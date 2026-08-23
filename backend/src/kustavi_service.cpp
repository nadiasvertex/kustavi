#include "kustavi_service.h"

#include "commit.h"
#include "exif.h"
#include "paths.h"
#include "similar.h"
#include "store.h"
#include "trips.h"
#include "version.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <filesystem>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace kustavi {

namespace {

/** Appends `values` to a proto repeated field. `*Add() = value` is the
 * sanctioned copy path (RepeatedPtrField::Add(const T&) is deleted). */
template <class Field, class Range>
auto append_range(Field *field, const Range &values) -> void {
  for (const auto &value : values) {
    *field->Add() = value;
  }
}
} // namespace

kustavi_service::kustavi_service(std::string auth_token)
    : auth_token_(std::move(auth_token)) {
  if (auth_token_.empty()) {
    spdlog::warn("no --token provided; gRPC auth validation is disabled");
  }
}

auto kustavi_service::check_auth(const grpc::ServerContext *context) const
    -> bool {
  if (auth_token_.empty()) {
    return true;
  }
  const auto &metadata = context->client_metadata();
  const auto it = metadata.find(std::string(k_auth_token_header));
  return it != metadata.end() && it->second == auth_token_;
}

auto kustavi_service::require_session() -> std::optional<grpc::Status> {
  if (!has_active_session_) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "no active session; call ScanFolder first");
  }
  return std::nullopt;
}

auto kustavi_service::try_begin_pass() -> std::optional<grpc::Status> {
  bool expected = false;
  if (!pass_active_.compare_exchange_strong(expected, true)) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "another pass is already running");
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

auto kustavi_service::GetInfo(grpc::ServerContext *context,
                              const GetInfoRequest *request,
                              GetInfoResponse *response) -> grpc::Status {
  (void)request;
  if (!check_auth(context)) {
    return unauthenticated();
  }
  response->set_version(std::string(kustavi::version));
  for (const auto ext : image::supported_image_extensions) {
    response->add_supported_formats(std::string(ext));
  }
  response->set_model_name(std::string(k_model_name));
  return grpc::Status::OK;
}

auto kustavi_service::Shutdown(grpc::ServerContext *context,
                               const ShutdownRequest *request,
                               ShutdownResponse *response) -> grpc::Status {
  (void)request;
  (void)response;
  if (!check_auth(context)) {
    return unauthenticated();
  }
  // Idempotent: only the first request flips the flag. serve() observes it
  // and stops the server off the worker pool (see is_shutdown_requested).
  if (shutdown_requested_.exchange(true) == false) {
    spdlog::info("shutdown requested via gRPC");
  }
  return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// Pass 1: scan & thumbnails
// ---------------------------------------------------------------------------

auto kustavi_service::ScanFolder(grpc::ServerContext *context,
                                 const ScanFolderRequest *request,
                                 grpc::ServerWriter<ScanEvent> *writer)
    -> grpc::Status {
  if (!check_auth(context)) {
    return unauthenticated();
  }
  if (const auto err = try_begin_pass()) {
    return *err;
  }
  pass_guard guard(pass_active_, true);

  const auto &folder_str = request->folder();
  if (folder_str.empty()) {
    return {grpc::StatusCode::INVALID_ARGUMENT, "folder is required"};
  }
  const fs::path folder{folder_str};
  std::error_code ec;
  if (!fs::is_directory(folder, ec) || ec) {
    return {grpc::StatusCode::NOT_FOUND,
            "folder does not exist: " + folder_str};
  }

  // A new scan starts a fresh session: discard the previous index and cache.
  try {
    session_db_.open(folder);
    session_db_.execute(
        "DELETE FROM images; DELETE FROM junk_flags; DELETE FROM "
        "quality_flags; DELETE FROM similar_groups; DELETE FROM "
        "user_decisions;");
  } catch (const std::exception &e) {
    return {grpc::StatusCode::INTERNAL,
            std::string("failed to start session: ") + e.what()};
  }

  const auto image_cache = config::image_cache_path(config::cache_path(folder));
  if (fs::exists(image_cache)) {
    std::error_code wipe_ec;
    fs::remove_all(image_cache, wipe_ec);
    fs::create_directories(image_cache, wipe_ec);
  }

  has_active_session_ = true;
  session_folder_ = folder;
  spdlog::info("scan started for '{}'", folder_str);

  event_queue<scan_event> queue;
  std::stop_source stop_source;
  std::exception_ptr producer_error;
  std::size_t next_progress_at = 250;

  std::thread producer = run_producer(
      queue, stop_source, producer_error, [&](std::stop_token st) -> void {
        const auto summary = image::execute_folder_ingestion_pass(
            session_db_, folder, request->recursive(), std::move(st),
            [&](std::size_t files_seen, std::size_t images_found,
                std::size_t) -> void {
              if (files_seen >= next_progress_at) {
                next_progress_at = files_seen + 250;
                queue.push(scan_progress_evt{.files_seen = files_seen,
                                             .images_found = images_found});
              }
            },
            [&](const image::ingestion_result &result) -> void {
              queue.push(scan_image_evt{result});
            });
        queue.push(scan_complete_evt{.images = summary.images_prepared,
                                     .skipped_files = summary.files_seen -
                                                      summary.images_found,
                                     .errors = summary.errors});
      });

  grpc::Status status = stream_pass(
      context, writer, queue, stop_source, [&](const scan_event &ev) -> bool {
        ScanEvent proto;
        std::visit(
            [&](const auto &e) -> auto {
              using evt = std::decay_t<decltype(e)>;
              if constexpr (std::is_same_v<evt, scan_progress_evt>) {
                auto *p = proto.mutable_progress();
                p->set_files_seen(static_cast<uint32_t>(e.files_seen));
                p->set_images_found(static_cast<uint32_t>(e.images_found));
              } else if constexpr (std::is_same_v<evt, scan_image_evt>) {
                auto *m = proto.mutable_image();
                const auto &r = e.result;
                m->set_id(r.relative_id);
                m->set_path(r.absolute_path.string());
                m->set_name(r.absolute_path.filename().string());
                m->set_width(static_cast<uint32_t>(r.original_width));
                m->set_height(static_cast<uint32_t>(r.original_height));
                m->set_size_bytes(static_cast<uint64_t>(r.size_bytes));
                if (r.taken_unix_ms.has_value()) {
                  m->set_taken_unix_ms(*r.taken_unix_ms);
                }
                if (r.latitude.has_value() && r.longitude.has_value()) {
                  auto *gps = m->mutable_gps();
                  gps->set_latitude(*r.latitude);
                  gps->set_longitude(*r.longitude);
                }
                m->set_thumbnail_path(r.working_path);
              } else {
                auto *c = proto.mutable_complete();
                c->set_images(static_cast<uint32_t>(e.images));
                c->set_skipped_files(static_cast<uint32_t>(e.skipped_files));
                append_range(c->mutable_errors(), e.errors);
              }
            },
            ev);
        return writer->Write(proto);
      });

  producer.join();
  if (const auto err = producer_error_status(producer_error)) {
    return *err;
  }
  spdlog::info("scan finished");
  return status;
}

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
  (void)request;
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

  std::vector<fs::path> paths;
  std::unordered_map<std::string, std::string> path_to_id;
  try {
    const auto records = store::get_image_records(session_db_);
    paths.reserve(records.size());
    path_to_id.reserve(records.size());
    for (const auto &record : records) {
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
  const auto thresholds = image::quality_thresholds{};

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

// ---------------------------------------------------------------------------
// Pass 3: model + junk (stubs until the vision pipeline lands)
// ---------------------------------------------------------------------------

auto kustavi_service::EnsureModel(grpc::ServerContext *context,
                                  const EnsureModelRequest *request,
                                  grpc::ServerWriter<ModelEvent> *writer)
    -> grpc::Status {
  (void)request;
  (void)writer;
  if (!check_auth(context)) {
    return unauthenticated();
  }
  return {grpc::StatusCode::UNIMPLEMENTED,
          "vision model pipeline is not implemented yet"};
}

auto kustavi_service::RunJunkPass(grpc::ServerContext *context,
                                  const RunJunkPassRequest *request,
                                  grpc::ServerWriter<JunkEvent> *writer)
    -> grpc::Status {
  (void)request;
  (void)writer;
  if (!check_auth(context)) {
    return unauthenticated();
  }
  return {grpc::StatusCode::UNIMPLEMENTED,
          "vision model pipeline is not implemented yet"};
}

// ---------------------------------------------------------------------------
// Pass 4: similar images
// ---------------------------------------------------------------------------

auto kustavi_service::RunSimilarPass(grpc::ServerContext *context,
                                     const RunSimilarPassRequest *request,
                                     grpc::ServerWriter<SimilarEvent> *writer)
    -> grpc::Status {
  (void)request;
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

  std::vector<fs::path> paths;
  std::unordered_map<std::string, std::string> path_to_id;
  std::unordered_map<std::string, double> sharpness_by_id;
  try {
    const auto records = store::get_image_records(session_db_);
    paths.reserve(records.size());
    path_to_id.reserve(records.size());
    for (const auto &record : records) {
      paths.push_back(record.absolute_path);
      path_to_id.emplace(record.absolute_path.string(), record.id);
    }
    sharpness_by_id = store::get_quality_scores(session_db_);
  } catch (const std::exception &e) {
    return {grpc::StatusCode::INTERNAL,
            std::string("failed to read session: ") + e.what()};
  }

  // Composite "bestness" score: normalized sharpness when the quality pass
  // ran, otherwise neutral. s / (s + 100) maps Laplacian variance to 0..1
  // monotonically (100 -> 0.5, 1000 -> ~0.91).
  const auto score_of = [&](const fs::path &path) -> double {
    const auto id_it = path_to_id.find(path.string());
    const auto score_it = id_it != path_to_id.end()
                              ? sharpness_by_id.find(id_it->second)
                              : sharpness_by_id.end();
    if (score_it == sharpness_by_id.end()) {
      return 0.5;
    }
    return score_it->second / (score_it->second + 100.0);
  };

  event_queue<similar_event> queue;
  std::stop_source stop_source;
  std::exception_ptr producer_error;

  std::thread producer = run_producer(
      queue, stop_source, producer_error,
      [&](const std::stop_token &st) -> void {
        const auto groups = image::find_similar_images(
            image::default_similarity_radius, paths,
            [&](std::size_t done) -> void {
              queue.push(
                  similar_progress_evt{.done = done, .total = paths.size()});
            });

        std::vector<std::tuple<uint32_t, std::string, std::string, double>>
            rows;
        uint32_t group_id = 0;
        for (const auto &group : groups) {
          if (group.size() < 2) {
            continue;
          }
          struct member {
            const fs::path *path;
            double score;
          };
          std::vector<member> members;
          members.reserve(group.size());
          for (const auto &path : group) {
            members.push_back({.path = &path, .score = score_of(path)});
          }
          std::ranges::sort(members,
                            [](const member &a, const member &b) -> bool {
                              if (a.score != b.score) {
                                return a.score > b.score;
                              }
                              return a.path->string() < b.path->string();
                            });

          similar_group_evt evt;
          evt.image_ids.reserve(members.size());
          evt.member_scores.reserve(members.size());
          const auto keeper_path = members.front().path;
          const std::string keeper_id = path_to_id.at(keeper_path->string());
          evt.keeper_id = keeper_id;
          for (const auto &m : members) {
            const auto id = path_to_id.at(m.path->string());
            evt.image_ids.push_back(id);
            evt.member_scores.push_back(m.score);
            rows.emplace_back(group_id, id, keeper_id, m.score);
          }
          group_id++;
          queue.push(std::move(evt));
        }

        // Persist the group membership.
        session_db_.begin_transaction();
        try {
          auto stmt = session_db_.prepare("INSERT INTO similar_groups "
                                          "(group_id, image_id, keeper_id, "
                                          "score, processed_at) VALUES "
                                          "(?, ?, ?, ?, "
                                          "strftime('%s','now'));");
          for (const auto &[group, id, keeper, score] : rows) {
            stmt.bind_int(1, static_cast<int>(group));
            stmt.bind_text(2, id);
            stmt.bind_text(3, keeper);
            stmt.bind_double(4, score);
            stmt.step();
            stmt.reset();
          }
          session_db_.commit_transaction();
        } catch (...) {
          session_db_.rollback_transaction();
          throw;
        }
        queue.push(similar_complete_evt{.groups = group_id,
                                        .total_images = paths.size()});
      });

  uint32_t next_group_id = 0;
  grpc::Status status = stream_pass(
      context, writer, queue, stop_source,
      [&](const similar_event &ev) -> bool {
        SimilarEvent proto;
        std::visit(
            [&](const auto &e) -> auto {
              using evt = std::decay_t<decltype(e)>;
              if constexpr (std::is_same_v<evt, similar_progress_evt>) {
                auto *p = proto.mutable_progress();
                p->set_done(static_cast<uint32_t>(e.done));
                p->set_total(static_cast<uint32_t>(e.total));
              } else if constexpr (std::is_same_v<evt, similar_group_evt>) {
                auto *g = proto.mutable_group();
                g->set_id(next_group_id++);
                g->set_recommended_keep_id(e.keeper_id);
                append_range(g->mutable_image_ids(), e.image_ids);
                append_range(g->mutable_member_scores(), e.member_scores);
              } else {
                auto *c = proto.mutable_complete();
                c->set_groups(static_cast<uint32_t>(e.groups));
                c->set_total_images(static_cast<uint32_t>(e.total_images));
              }
            },
            ev);
        return writer->Write(proto);
      });

  producer.join();
  if (const auto err = producer_error_status(producer_error)) {
    return *err;
  }
  spdlog::info("similar pass finished");
  return status;
}

// ---------------------------------------------------------------------------
// Pass 5: trips
// ---------------------------------------------------------------------------

auto kustavi_service::RunTripsPass(grpc::ServerContext *context,
                                   const RunTripsPassRequest *request,
                                   grpc::ServerWriter<TripsEvent> *writer)
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

  std::vector<trip_member> members;
  try {
    const auto records = store::get_image_records(session_db_);
    members.reserve(records.size());
    for (const auto &record : records) {
      trip_member member;
      member.id = record.id;
      member.taken_unix_ms = record.taken_unix_ms;
      member.latitude = record.latitude;
      member.longitude = record.longitude;
      members.push_back(std::move(member));
    }
  } catch (const std::exception &e) {
    return {grpc::StatusCode::INTERNAL,
            std::string("failed to read session: ") + e.what()};
  }

  // proto3 zeroes unset ints, so treat non-positive values as "use default".
  const int max_gap_hours =
      request->max_gap_hours() > 0 ? request->max_gap_hours() : 48;
  const int max_distance_km =
      request->max_distance_km() > 0 ? request->max_distance_km() : 300;

  event_queue<trips_event> queue;
  std::stop_source stop_source;
  std::exception_ptr producer_error;

  std::thread producer = run_producer(
      queue, stop_source, producer_error,
      [&](const std::stop_token &st) -> void {
        (void)st;
        const auto result = find_trips(members, max_gap_hours, max_distance_km);
        queue.push(trips_progress_evt{.done = result.trips.size(),
                                      .total = result.trips.size()});
        for (const auto &trip : result.trips) {
          queue.push(
              trips_result_evt{.id = trip.id,
                               .start_unix_ms = trip.start_unix_ms,
                               .end_unix_ms = trip.end_unix_ms,
                               .image_ids = trip.image_ids,
                               .centroid_latitude = trip.centroid_latitude,
                               .centroid_longitude = trip.centroid_longitude});
        }
        queue.push(trips_complete_evt{.trips = result.trips.size(),
                                      .unassigned = result.unassigned});
      });

  grpc::Status status = stream_pass(
      context, writer, queue, stop_source, [&](const trips_event &ev) -> bool {
        TripsEvent proto;
        std::visit(
            [&](const auto &e) -> auto {
              using evt = std::decay_t<decltype(e)>;
              if constexpr (std::is_same_v<evt, trips_progress_evt>) {
                auto *p = proto.mutable_progress();
                p->set_done(static_cast<uint32_t>(e.done));
                p->set_total(static_cast<uint32_t>(e.total));
              } else if constexpr (std::is_same_v<evt, trips_result_evt>) {
                auto *t = proto.mutable_trip();
                t->set_id(e.id);
                t->set_start_unix_ms(e.start_unix_ms);
                t->set_end_unix_ms(e.end_unix_ms);
                append_range(t->mutable_image_ids(), e.image_ids);
                if (e.centroid_latitude.has_value() &&
                    e.centroid_longitude.has_value()) {
                  auto *centroid = t->mutable_centroid();
                  centroid->set_latitude(*e.centroid_latitude);
                  centroid->set_longitude(*e.centroid_longitude);
                }
              } else {
                auto *c = proto.mutable_complete();
                c->set_trips(static_cast<uint32_t>(e.trips));
                c->set_unassigned(static_cast<uint32_t>(e.unassigned));
              }
            },
            ev);
        return writer->Write(proto);
      });

  producer.join();
  if (const auto err = producer_error_status(producer_error)) {
    return *err;
  }
  spdlog::info("trips pass finished");
  return status;
}

// ---------------------------------------------------------------------------
// Pass 6: commit
// ---------------------------------------------------------------------------

auto kustavi_service::Commit(grpc::ServerContext *context,
                             const CommitRequest *request,
                             grpc::ServerWriter<CommitEvent> *writer)
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

  const auto &destination_str = request->destination();
  if (destination_str.empty()) {
    return {grpc::StatusCode::INVALID_ARGUMENT, "destination is required"};
  }
  const fs::path destination{destination_str};
  if (!destination.is_absolute()) {
    return {grpc::StatusCode::INVALID_ARGUMENT,
            "destination must be an absolute path"};
  }

  std::vector<commit_source> sources;
  std::vector<std::string> errors;
  try {
    const auto records = store::get_image_records(session_db_);
    std::unordered_map<std::string, fs::path> id_to_path;
    id_to_path.reserve(records.size());
    for (const auto &record : records) {
      id_to_path.emplace(record.id, record.absolute_path);
    }
    sources.reserve(request->keep_ids_size());
    for (const auto &id : request->keep_ids()) {
      const auto it = id_to_path.find(id);
      if (it == id_to_path.end()) {
        errors.push_back(id + ": not found in session");
        continue;
      }
      sources.push_back({.id = id, .path = it->second});
    }
  } catch (const std::exception &e) {
    return {grpc::StatusCode::INTERNAL,
            std::string("failed to read session: ") + e.what()};
  }

  event_queue<commit_event> queue;
  std::stop_source stop_source;
  std::exception_ptr producer_error;

  std::thread producer = run_producer(
      queue, stop_source, producer_error,
      [&](const std::stop_token &st) -> void {
        auto summary =
            commit_files(session_folder_, destination, sources, st,
                         [&](std::size_t done, std::size_t total,
                             const fs::path &current) -> void {
                           queue.push(commit_progress_evt{
                               .done = done,
                               .total = total,
                               .current_name = current.filename().string()});
                         });
        for (const auto &error : errors) {
          summary.errors.push_back(error);
        }
        queue.push(commit_complete_evt{.copied = summary.copied,
                                       .skipped = summary.skipped,
                                       .errors = std::move(summary.errors)});
      });

  grpc::Status status = stream_pass(
      context, writer, queue, stop_source, [&](const commit_event &ev) -> bool {
        CommitEvent proto;
        std::visit(
            [&](const auto &e) -> auto {
              using evt = std::decay_t<decltype(e)>;
              if constexpr (std::is_same_v<evt, commit_progress_evt>) {
                auto *p = proto.mutable_progress();
                p->set_done(static_cast<uint32_t>(e.done));
                p->set_total(static_cast<uint32_t>(e.total));
                p->set_current_name(e.current_name);
              } else {
                auto *c = proto.mutable_complete();
                c->set_copied(static_cast<uint32_t>(e.copied));
                c->set_skipped(static_cast<uint32_t>(e.skipped));
                append_range(c->mutable_errors(), e.errors);
              }
            },
            ev);
        return writer->Write(proto);
      });

  producer.join();
  if (const auto err = producer_error_status(producer_error)) {
    return *err;
  }
  spdlog::info("commit finished");
  return status;
}

} // namespace kustavi
