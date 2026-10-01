#include "kustavi_service.h"

#include "paths.h"
#include "store/store.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
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
  // is_blurry() already suppresses itself on a badly exposed frame, where the
  // sharpness measure is unreliable.
  if (image::is_blurry(metrics, thresholds)) {
    reasons.push_back(BLURRY);
  }
  if (image::is_underexposed(metrics, thresholds)) {
    reasons.push_back(UNDER_EXPOSED);
  }
  if (image::is_overexposed(metrics, thresholds)) {
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

/** The proto reasons packed into a bitmask (BLURRY=1, UNDER=2, OVER=4), so a
 *  resumed session can restore the review without recomputing them. */
auto reasons_bitmask(const std::vector<QualityReason> &reasons) -> int {
  int mask = 0;
  for (const auto reason : reasons) {
    switch (reason) {
    case BLURRY:
      mask |= 1;
      break;
    case UNDER_EXPOSED:
      mask |= 2;
      break;
    case OVER_EXPOSED:
      mask |= 4;
      break;
    default:
      break;
    }
  }
  return mask;
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
  record_step(1); // WizardStep.quality

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

  const std::unordered_set<std::string> scope(request->scope_image_ids().begin(),
                                              request->scope_image_ids().end());
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
      if (!scope.empty() && !scope.contains(record.id)) {
        continue;
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
                                          .sharpness = m.focus_peak_variance,
                                          .exposure_score = exposure_score(m)});
            });

        // Persist the metrics (plus the packed reasons + peak sharpness so a
        // resume can restore the review verbatim), then report.
        std::size_t flagged = 0;
        session_db_.begin_transaction();
        try {
          auto stmt = session_db_.prepare(
              "INSERT OR REPLACE INTO quality_flags (image_id, laplacian, "
              "underexposed, overexposed, focus_peak, reasons, processed_at) "
              "VALUES (?, ?, ?, ?, ?, ?, strftime('%s','now'));");
          for (const auto &m : metrics) {
            if (!m.valid) {
              continue;
            }
            const auto id_it = path_to_id.find(m.path.string());
            if (id_it == path_to_id.end()) {
              continue;
            }
            const int mask = reasons_bitmask(quality_reasons(m, thresholds));
            if (mask != 0) {
              flagged++;
            }
            stmt.bind_text(1, id_it->second);
            stmt.bind_double(2, m.laplacian_variance);
            stmt.bind_double(3, m.underexposed_ratio);
            stmt.bind_double(4, m.overexposed_ratio);
            stmt.bind_double(5, m.focus_peak_variance);
            stmt.bind_int(6, mask);
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
  if (status.ok()) {
    record_run_complete(1, request->batch_key()); // WizardStep.quality
  }
  spdlog::info("quality pass finished");
  return status;
}

auto kustavi_service::PreviewQualityThresholds(
    grpc::ServerContext *context, const PreviewQualityThresholdsRequest *request,
    PreviewQualityThresholdsResponse *response) -> grpc::Status {
  if (!check_auth(context)) {
    return unauthenticated();
  }
  if (const auto err = require_session()) {
    return *err;
  }

  const auto &in = request->thresholds();
  if (in.blur_threshold() <= 0) {
    return {grpc::StatusCode::INVALID_ARGUMENT,
            "blur_threshold must be a positive number"};
  }
  if (in.underexposed_threshold() < 0 || in.underexposed_threshold() >= 1 ||
      in.overexposed_threshold() < 0 || in.overexposed_threshold() >= 1) {
    return {grpc::StatusCode::INVALID_ARGUMENT,
            "exposure thresholds must be in [0, 1)"};
  }

  image::quality_thresholds thresholds;
  thresholds.blur_threshold = in.blur_threshold();
  thresholds.underexposed_threshold = in.underexposed_threshold();
  thresholds.overexposed_threshold = in.overexposed_threshold();

  try {
    auto stmt = session_db_.prepare(
        "SELECT laplacian, underexposed, overexposed, focus_peak "
        "FROM quality_flags;");
    std::uint32_t total = 0;
    std::uint32_t flagged = 0;
    std::uint32_t blurry = 0;
    std::uint32_t under = 0;
    std::uint32_t over = 0;
    while (stmt.step() == SQLITE_ROW) {
      image::local_image_metrics m;
      m.valid = true;
      m.laplacian_variance = sqlite3_column_double(stmt.raw(), 0);
      m.underexposed_ratio = sqlite3_column_double(stmt.raw(), 1);
      m.overexposed_ratio = sqlite3_column_double(stmt.raw(), 2);
      m.focus_peak_variance = sqlite3_column_double(stmt.raw(), 3);
      total++;
      const bool is_blur = image::is_blurry(m, thresholds);
      const bool is_under = image::is_underexposed(m, thresholds);
      const bool is_over = image::is_overexposed(m, thresholds);
      blurry += is_blur ? 1U : 0U;
      under += is_under ? 1U : 0U;
      over += is_over ? 1U : 0U;
      flagged += (is_blur || is_under || is_over) ? 1U : 0U;
    }
    response->set_total(total);
    response->set_flagged(flagged);
    response->set_blurry(blurry);
    response->set_under_exposed(under);
    response->set_over_exposed(over);
  } catch (const std::exception &e) {
    return {grpc::StatusCode::INTERNAL,
            std::string("failed to preview thresholds: ") + e.what()};
  }
  return grpc::Status::OK;
}
} // namespace kustavi
