#include "kustavi_service.h"

#include "paths.h"
#include "store/store.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kustavi {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Pass 2: blur / exposure
// ---------------------------------------------------------------------------

namespace {

/** Maps per-image metrics to the proto flag reasons. */
auto quality_reasons(const image::local_image_metrics &metrics,
                     const image::quality_thresholds &thresholds)
    -> std::vector<QualityReason> {
  std::vector<QualityReason> reasons;
  if (metrics.laplacian_variance < thresholds.blur_threshold) {
    reasons.push_back(BLURRY);
  }
  if (metrics.underexposed_ratio > thresholds.underexposed_threshold) {
    reasons.push_back(UNDER_EXPOSED);
  }
  if (metrics.overexposed_ratio > thresholds.overexposed_threshold) {
    reasons.push_back(OVER_EXPOSED);
  }
  return reasons;
}

/** 0..1 where 0.5 is ideal and lower is a worse exposure balance. */
auto exposure_score(const image::local_image_metrics &metrics) -> double {
  const double clipped =
      std::max(metrics.underexposed_ratio, metrics.overexposed_ratio);
  return std::clamp(0.5 - clipped, 0.0, 0.5);
}

} // namespace

auto kustavi_service::RunQualityPass(grpc::ServerContext *context,
                                     const RunQualityPassRequest *request,
                                     grpc::ServerWriter<QualityEvent> *writer)
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

  double blur = request->blur_threshold();
  double under = request->underexposed_threshold();
  double over = request->overexposed_threshold();
  if (blur <= 0) {
    return {grpc::StatusCode::INVALID_ARGUMENT,
            "blur_threshold must be a positive number"};
  }
  if (under < 0 || under >= 1) {
    return {grpc::StatusCode::INVALID_ARGUMENT,
            "underexposed_threshold must be in [0, 1)"};
  }
  if (over < 0 || over >= 1) {
    return {grpc::StatusCode::INVALID_ARGUMENT,
            "overexposed_threshold must be in [0, 1)"};
  }

  image::quality_thresholds thresholds;
  thresholds.blur_threshold = blur;
  thresholds.underexposed_threshold = under;
  thresholds.overexposed_threshold = over;

  std::vector<fs::path> paths;
  std::unordered_map<std::string, std::string> path_to_id;
  try {
    const auto records = store::get_image_records(session_db_);
    paths.reserve(records.size());
    path_to_id.reserve(records.size());
    for (const auto &record : records) {
      if (record.kind != image::media_kind_photo) {
        continue; // blur/exposure metrics don't apply to videos
      }
      paths.push_back(record.absolute_path);
      path_to_id.emplace(record.absolute_path.string(), record.id);
    }
  } catch (const std::exception &e) {
    return {grpc::StatusCode::INTERNAL,
            std::string("failed to read session: ") + e.what()};
  }

  event_queue<quality_event> queue;
  std::stop_source stop_source;
  std::exception_ptr producer_error;

  std::thread producer = run_producer(
      queue, stop_source, producer_error, [&](std::stop_token st) -> void {
        const auto metrics = image::analyze_images(
            thresholds, paths, std::move(st),
            [&](std::size_t done, std::size_t total) -> void {
              queue.push(quality_progress_evt{.done = done, .total = total});
            },
            [&](const image::local_image_metrics &m) -> void {
              const auto reasons = quality_reasons(m, thresholds);
              if (reasons.empty()) {
                return;
              }
              auto id_it = path_to_id.find(m.path.string());
              if (id_it == path_to_id.end()) {
                return;
              }
              queue.push(quality_flag_evt{.image_id = id_it->second,
                                          .reasons = reasons,
                                          .sharpness = m.laplacian_variance,
                                          .exposure_score = exposure_score(m)});
            });

        // Persist the metrics, then report.
        std::size_t flagged = 0;
        session_db_.begin_transaction();
        try {
          auto stmt = session_db_.prepare("INSERT OR REPLACE INTO "
                                          "quality_flags (image_id, "
                                          "laplacian, underexposed, "
                                          "overexposed, processed_at) "
                                          "VALUES (?, ?, ?, ?, "
                                          "strftime('%s','now'));");
          for (const auto &m : metrics) {
            if (!m.valid) {
              continue;
            }
            const auto id_it = path_to_id.find(m.path.string());
            if (id_it == path_to_id.end()) {
              continue;
            }
            if (!quality_reasons(m, thresholds).empty()) {
              flagged++;
            }
            stmt.bind_text(1, id_it->second);
            stmt.bind_double(2, m.laplacian_variance);
            stmt.bind_double(3, m.underexposed_ratio);
            stmt.bind_double(4, m.overexposed_ratio);
            stmt.step();
            stmt.reset();
          }
          session_db_.commit_transaction();
        } catch (...) {
          session_db_.rollback_transaction();
          throw;
        }
        queue.push(
            quality_complete_evt{.flagged = flagged, .total = metrics.size()});
      });

  grpc::Status status = stream_pass(
      context, writer, queue, stop_source,
      [&](const quality_event &ev) -> bool {
        QualityEvent proto;
        std::visit(
            [&](const auto &e) -> auto {
              using evt = std::decay_t<decltype(e)>;
              if constexpr (std::is_same_v<evt, quality_progress_evt>) {
                auto *p = proto.mutable_progress();
                p->set_done(static_cast<uint32_t>(e.done));
                p->set_total(static_cast<uint32_t>(e.total));
              } else if constexpr (std::is_same_v<evt, quality_flag_evt>) {
                auto *f = proto.mutable_flag();
                f->set_image_id(e.image_id);
                for (const auto reason : e.reasons) {
                  f->add_reasons(reason);
                }
                f->set_sharpness(e.sharpness);
                f->set_exposure_score(e.exposure_score);
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
  spdlog::info("quality pass finished");
  return status;
}
} // namespace kustavi
