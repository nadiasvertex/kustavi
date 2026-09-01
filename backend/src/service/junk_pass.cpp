#include "kustavi_service.h"

#include "net/model_download.h"
#include "pass/junk.h"
#include "paths.h"
#include "store/store.h"

#include <spdlog/spdlog.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace kustavi {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Pass 3: model download (EnsureModel) + vision junk classification (RunJunkPass)
//
// Model: Moondream 2 (SigLIP + Phi-1.5, llava-style) from the ggml-org test
// repo. The spec names "Moondream-3.1", but Moondream 3 has no llama.cpp/mtmd
// projector; Moondream 2 is the supported vision model.
// ---------------------------------------------------------------------------

namespace {

constexpr std::string_view k_model_display_name = "moondream2";
constexpr std::string_view k_hf_base =
    "https://huggingface.co/ggml-org/moondream2-20250414-GGUF/resolve/main/";

auto text_model_asset() -> net::remote_asset {
  return {
      .url = std::string(k_hf_base) +
             "moondream2-text-model-f16_ct-vicuna.gguf",
      .dest = config::models_path() / "moondream2-text-model-f16.gguf",
      .sha256_hex =
          "925bcb666baf69ed747e26121af287b16ae7764483be9548b1382f29783689a5",
      .size_bytes = 2'839'535'072ULL,
  };
}

auto mmproj_asset() -> net::remote_asset {
  return {
      .url = std::string(k_hf_base) + "moondream2-mmproj-f16-20250414.gguf",
      .dest = config::models_path() / "moondream2-mmproj-f16.gguf",
      .sha256_hex =
          "4cc1cb3660d87ff56432ebeb7884ad35d67c48c7b9f6b2856f305e39c38eed8f",
      .size_bytes = 909'777'984ULL,
  };
}

/** Image ids that already carry a junk_flags row (resume across pauses). */
auto classified_ids(database &db) -> std::unordered_set<std::string> {
  std::unordered_set<std::string> ids;
  auto stmt = db.prepare("SELECT image_id FROM junk_flags;");
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

auto kustavi_service::EnsureModel(grpc::ServerContext *context,
                                  const EnsureModelRequest *request,
                                  grpc::ServerWriter<ModelEvent> *writer)
    -> grpc::Status {
  (void)request;
  if (!check_auth(context)) {
    return unauthenticated();
  }

  const auto text = text_model_asset();
  const auto mmproj = mmproj_asset();
  const std::uint64_t total_bytes = text.size_bytes + mmproj.size_bytes;

  event_queue<model_event> queue;
  std::stop_source stop_source;
  std::exception_ptr producer_error;

  std::thread producer = run_producer(
      queue, stop_source, producer_error, [&](const std::stop_token &st) -> void {
        // EnsureModel is exempt from the single-pass lock but must not race
        // another EnsureModel. Wait for the model lock without blocking a
        // client cancellation.
        std::unique_lock<std::mutex> lock(model_mutex_, std::defer_lock);
        while (!lock.try_lock()) {
          if (st.stop_requested()) {
            return;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        const std::array<net::remote_asset, 2> assets{text, mmproj};
        std::uint64_t base = 0;
        for (const auto &asset : assets) {
          if (net::asset_ready(asset)) {
            base += asset.size_bytes;
            continue;
          }
          const auto result = net::download_asset(
              asset, st, [&](const net::download_progress &p) -> void {
                queue.push(model_progress_evt{
                    .done_bytes = base + p.done_bytes,
                    .total_bytes = total_bytes,
                    .speed_bps = p.speed_bps,
                });
              });
          if (!result) {
            if (st.stop_requested()) {
              return; // cancelled: the .part file stays for a later resume
            }
            throw std::runtime_error(result.error());
          }
          base += asset.size_bytes;
        }

        model_ready_ = true;
        queue.push(model_ready_evt{
            .model_name = std::string(k_model_display_name),
            .size_bytes = total_bytes,
        });
      });

  grpc::Status status = stream_pass(
      context, writer, queue, stop_source,
      [&](const model_event &ev) -> bool {
        ModelEvent proto;
        std::visit(
            [&](const auto &e) -> void {
              using evt = std::decay_t<decltype(e)>;
              if constexpr (std::is_same_v<evt, model_progress_evt>) {
                auto *p = proto.mutable_progress();
                p->set_done_bytes(e.done_bytes);
                p->set_total_bytes(e.total_bytes);
                p->set_speed_bps(e.speed_bps);
              } else {
                auto *r = proto.mutable_ready();
                r->set_model_name(e.model_name);
                r->set_size_bytes(e.size_bytes);
              }
            },
            ev);
        return writer->Write(proto);
      });

  producer.join();
  if (const auto err = producer_error_status(producer_error)) {
    return *err;
  }
  spdlog::info("vision model is ready");
  return status;
}

auto kustavi_service::RunJunkPass(grpc::ServerContext *context,
                                  const RunJunkPassRequest *request,
                                  grpc::ServerWriter<JunkEvent> *writer)
    -> grpc::Status {
  (void)request;
  if (!check_auth(context)) {
    return unauthenticated();
  }
  if (const auto err = require_session()) {
    return *err;
  }

  const auto text = text_model_asset();
  const auto mmproj = mmproj_asset();
  {
    std::scoped_lock lock(model_mutex_);
    if (!model_ready_ &&
        !(net::asset_ready(text) && net::asset_ready(mmproj))) {
      return {grpc::StatusCode::FAILED_PRECONDITION,
              "vision model is not ready; call EnsureModel first"};
    }
    model_ready_ = true;
  }

  if (const auto err = try_begin_pass()) {
    return *err;
  }
  pass_guard guard(pass_active_, true);

  std::vector<store::image_record> records;
  std::unordered_set<std::string> already_done;
  try {
    records = store::get_image_records(session_db_);
    already_done = classified_ids(session_db_);
  } catch (const std::exception &e) {
    return {grpc::StatusCode::INTERNAL,
            std::string("failed to read session: ") + e.what()};
  }

  event_queue<junk_event> queue;
  std::stop_source stop_source;
  std::exception_ptr producer_error;

  std::thread producer = run_producer(
      queue, stop_source, producer_error, [&](const std::stop_token &st) -> void {
        auto classifier =
            image::junk_classifier::load(text.dest, mmproj.dest);
        if (!classifier) {
          throw std::runtime_error(classifier.error());
        }

        struct scored_row {
          std::string id;
          bool is_junk = false;
          std::string reason;
          double confidence = 0.0;
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
            queue.push(junk_progress_evt{.done = done, .total = total});
            continue;
          }

          const fs::path &image = record.working_path.empty()
                                      ? record.absolute_path
                                      : record.working_path;
          const auto result = classifier->classify(image);
          if (result.valid) {
            rows.push_back({.id = record.id,
                            .is_junk = result.is_junk,
                            .reason = result.reason,
                            .confidence = result.confidence});
            if (result.is_junk) {
              ++flagged;
              queue.push(junk_flag_evt{.image_id = record.id,
                                       .reason = result.reason,
                                       .confidence = result.confidence});
            }
          }
          queue.push(junk_progress_evt{.done = done, .total = total});
        }

        session_db_.begin_transaction();
        try {
          auto stmt = session_db_.prepare(
              "INSERT OR REPLACE INTO junk_flags (image_id, is_junk, reason, "
              "confidence, processed_at) VALUES (?, ?, ?, ?, "
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
            stmt.step();
            stmt.reset();
          }
          session_db_.commit_transaction();
        } catch (...) {
          session_db_.rollback_transaction();
          throw;
        }

        queue.push(junk_complete_evt{.flagged = flagged, .total = total});
      });

  grpc::Status status = stream_pass(
      context, writer, queue, stop_source,
      [&](const junk_event &ev) -> bool {
        JunkEvent proto;
        std::visit(
            [&](const auto &e) -> void {
              using evt = std::decay_t<decltype(e)>;
              if constexpr (std::is_same_v<evt, junk_progress_evt>) {
                auto *p = proto.mutable_progress();
                p->set_done(static_cast<uint32_t>(e.done));
                p->set_total(static_cast<uint32_t>(e.total));
              } else if constexpr (std::is_same_v<evt, junk_flag_evt>) {
                auto *f = proto.mutable_flag();
                f->set_image_id(e.image_id);
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
  spdlog::info("junk pass finished");
  return status;
}
} // namespace kustavi
