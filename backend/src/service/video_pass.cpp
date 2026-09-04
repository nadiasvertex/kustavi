#include "kustavi_service.h"

#include "net/model_download.h"
#include "pass/junk.h"
#include "pass/video.h"
#include "paths.h"
#include "store/store.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace kustavi {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Pass 6: video quality (duration / blur / motion / corruption, plus a reuse
// of the photo junk classifier against a couple of sampled frames)
// ---------------------------------------------------------------------------

namespace {

// Mirrors junk_pass.cpp's model asset descriptors; the video pass loads the
// same Qwen2.5-VL classifier to catch non-photographic sampled frames (e.g.
// screen recordings).
constexpr std::string_view k_hf_base =
    "https://huggingface.co/ggml-org/Qwen2.5-VL-3B-Instruct-GGUF/resolve/main/";

auto text_model_asset() -> net::remote_asset {
  return {
      .url = std::string(k_hf_base) + "Qwen2.5-VL-3B-Instruct-Q4_K_M.gguf",
      .dest = config::models_path() / "qwen2.5-vl-3b-instruct-q4_k_m.gguf",
      .sha256_hex =
          "d02fe9b69ad8cadbbd228e387667af66612c44bed29ffc8eb1e7caf9ac486c12",
      .size_bytes = 1'929'901'056ULL,
  };
}

auto mmproj_asset() -> net::remote_asset {
  return {
      .url = std::string(k_hf_base) + "mmproj-Qwen2.5-VL-3B-Instruct-f16.gguf",
      .dest = config::models_path() / "qwen2.5-vl-3b-mmproj-f16.gguf",
      .sha256_hex =
          "b9160fe9d814d1fadf68395677468534778b39ac33c2e7561b7b218626e60d5e",
      .size_bytes = 1'338'428'128ULL,
  };
}

/** Video ids that already carry a video_flags row (resume across pauses). */
auto classified_video_ids(database &db) -> std::unordered_set<std::string> {
  std::unordered_set<std::string> ids;
  auto stmt = db.prepare("SELECT video_id FROM video_flags;");
  while (stmt.step() == SQLITE_ROW) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) sqlite C API
    const auto *id =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt.raw(), 0));
    if (id != nullptr) {
      ids.emplace(id);
    }
  }
  return ids;
}

} // namespace

auto kustavi_service::RunVideoPass(grpc::ServerContext *context,
                                   const RunVideoPassRequest *request,
                                   grpc::ServerWriter<VideoEvent> *writer)
    -> grpc::Status {
  if (!check_auth(context)) {
    return unauthenticated();
  }
  if (const auto err = require_session()) {
    return *err;
  }
  if (const auto err = try_begin_pass()) {
    return *err;
  }
  pass_guard guard(pass_active_, true);

  // The vision classifier is optional: without it the pass still catches
  // too-short/corrupt/blurry/static clips, just not non-photographic content.
  const auto text = text_model_asset();
  const auto mmproj = mmproj_asset();
  const bool vision_available =
      net::asset_ready(text) && net::asset_ready(mmproj);

  std::vector<store::image_record> records;
  std::unordered_set<std::string> already_done;
  try {
    for (auto &record : store::get_image_records(session_db_)) {
      if (record.kind == image::media_kind_video) {
        records.push_back(std::move(record));
      }
    }
    already_done = classified_video_ids(session_db_);
  } catch (const std::exception &e) {
    return {grpc::StatusCode::INTERNAL,
            std::string("failed to read session: ") + e.what()};
  }

  for (const auto &id : request->skip_video_ids()) {
    already_done.insert(id);
  }

  event_queue<video_event> queue;
  std::stop_source stop_source;
  std::exception_ptr producer_error;

  std::thread producer = run_producer(
      queue, stop_source, producer_error,
      [&](const std::stop_token &st) -> void {
        std::optional<image::junk_classifier> classifier;
        if (vision_available) {
          auto loaded = image::junk_classifier::load(text.dest, mmproj.dest);
          if (loaded) {
            classifier.emplace(std::move(*loaded));
          } else {
            spdlog::warn("video pass: vision classifier unavailable ({}); "
                         "screen-recording detection disabled",
                         loaded.error());
          }
        }

        image::video_thresholds thresholds;

        struct scored_row {
          std::string id;
          bool is_junk = false;
          std::string reason;
          double confidence = 0.0;
          std::int64_t duration_ms = 0;
        };
        std::vector<scored_row> rows;
        const std::size_t total = records.size();
        std::size_t done = 0;
        std::size_t flagged = 0;

        for (const auto &record : records) {
          if (st.stop_requested()) {
            return;
          }
          ++done;
          if (already_done.contains(record.id)) {
            queue.push(video_progress_evt{.done = done, .total = total});
            continue;
          }

          const fs::path &video = record.absolute_path;
          const auto metrics = image::analyze_video(
              video, thresholds, classifier ? &*classifier : nullptr);
          const auto [is_junk, reason] = image::is_flagged(metrics, thresholds);

          const double confidence =
              !metrics.junk_reason.empty() && reason == metrics.junk_reason
                  ? metrics.junk_confidence
                  : (is_junk ? 1.0 : 0.0);

          rows.push_back({.id = record.id,
                          .is_junk = is_junk,
                          .reason = reason,
                          .confidence = confidence,
                          .duration_ms = metrics.duration_ms});
          if (is_junk) {
            ++flagged;
            queue.push(video_flag_evt{.video_id = record.id,
                                      .reason = reason,
                                      .confidence = confidence});
          }
          queue.push(video_progress_evt{.done = done, .total = total});
        }

        session_db_.begin_transaction();
        try {
          auto stmt = session_db_.prepare(
              "INSERT OR REPLACE INTO video_flags (video_id, is_junk, reason, "
              "confidence, duration_ms, processed_at) VALUES (?, ?, ?, ?, ?, "
              "strftime('%s','now'));");
          for (const auto &row : rows) {
            stmt.bind_text(1, row.id);
            stmt.bind_int(2, row.is_junk ? 1 : 0);
            if (row.reason.empty()) {
              stmt.bind_null(3);
            } else {
              stmt.bind_text(3, row.reason);
            }
            stmt.bind_double(4, row.confidence);
            stmt.bind_int64(5, row.duration_ms);
            stmt.step();
            stmt.reset();
          }
          session_db_.commit_transaction();
        } catch (...) {
          session_db_.rollback_transaction();
          throw;
        }

        queue.push(video_complete_evt{.flagged = flagged, .total = total});
      });

  grpc::Status status = stream_pass(
      context, writer, queue, stop_source, [&](const video_event &ev) -> bool {
        VideoEvent proto;
        std::visit(
            [&](const auto &e) -> void {
              using evt = std::decay_t<decltype(e)>;
              if constexpr (std::is_same_v<evt, video_progress_evt>) {
                auto *p = proto.mutable_progress();
                p->set_done(static_cast<uint32_t>(e.done));
                p->set_total(static_cast<uint32_t>(e.total));
              } else if constexpr (std::is_same_v<evt, video_flag_evt>) {
                auto *f = proto.mutable_flag();
                f->set_video_id(e.video_id);
                f->set_reason(e.reason);
                f->set_confidence(e.confidence);
              } else {
                auto *c = proto.mutable_complete();
                c->set_flagged(static_cast<uint32_t>(e.flagged));
                c->set_total(static_cast<uint32_t>(e.total));
              }
            },
            ev);
        return writer->Write(proto);
      });

  producer.join();
  if (const auto err = producer_error_status(producer_error)) {
    return *err;
  }
  spdlog::info("video pass finished");
  return status;
}
} // namespace kustavi
