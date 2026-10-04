#include "kustavi_service.h"

#include "pass/repair.h"
#include "store/store.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kustavi {

namespace fs = std::filesystem;

namespace {

/** File modification time as Unix milliseconds, or nullopt when unreadable. */
auto modified_unix_ms(const fs::path &path) -> std::optional<std::int64_t> {
  std::error_code ec;
  const auto file_time = fs::last_write_time(path, ec);
  if (ec) {
    return std::nullopt;
  }
  // file_clock has no portable conversion to system_clock before C++20 is
  // fully implemented everywhere; rebase against "now" on both clocks.
  const auto system_time =
      std::chrono::system_clock::now() +
      std::chrono::duration_cast<std::chrono::system_clock::duration>(
          file_time - fs::file_time_type::clock::now());
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             system_time.time_since_epoch())
      .count();
}

auto changed(const store::image_record &record, const repaired_metadata &now)
    -> bool {
  return record.taken_unix_ms != now.taken_unix_ms ||
         record.latitude != now.latitude || record.longitude != now.longitude ||
         record.date_source != now.date_source ||
         record.gps_source != now.gps_source;
}

/** True when the values do not come straight from the file's EXIF block. */
auto synthetic(const repaired_metadata &now) -> bool {
  return (!now.date_source.empty() && now.date_source != "exif") ||
         now.gps_source == "inferred";
}

} // namespace

// ---------------------------------------------------------------------------
// Metadata repair: fill missing dates and GPS, propose camera clock offsets
// ---------------------------------------------------------------------------

auto kustavi_service::RunRepairPass(grpc::ServerContext *context,
                                    const RunRepairPassRequest *request,
                                    grpc::ServerWriter<RepairEvent> *writer)
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

  repair_params params;
  if (request->gps_window_minutes() > 0) {
    params.gps_window_minutes = request->gps_window_minutes();
  }
  for (const auto &offset : request->apply_offsets()) {
    params.offset_minutes_by_camera[offset.camera()] = offset.offset_minutes();
  }

  std::vector<store::image_record> records;
  try {
    records = store::get_image_records(session_db_);
  } catch (const std::exception &e) {
    return {grpc::StatusCode::INTERNAL,
            std::string("failed to read session: ") + e.what()};
  }

  event_queue<repair_event> queue;
  std::stop_source stop_source;
  std::exception_ptr producer_error;

  std::thread producer = run_producer(
      queue, stop_source, producer_error,
      [&](const std::stop_token &st) -> void {
        std::vector<repair_input> inputs;
        inputs.reserve(records.size());
        for (const auto &record : records) {
          if (st.stop_requested()) {
            return;
          }
          // A repaired position is not the file's own; start from EXIF only.
          const bool own_gps = record.gps_source != "inferred";
          inputs.push_back(
              {.id = record.id,
               .file_name = record.file_name,
               .camera = record.camera,
               .exif_taken_ms = record.taken_exif_ms,
               .modified_ms = modified_unix_ms(record.absolute_path),
               .exif_latitude = own_gps ? record.latitude : std::nullopt,
               .exif_longitude = own_gps ? record.longitude : std::nullopt});
        }
        queue.push(repair_progress_evt{.done = 0, .total = records.size()});

        const auto suggestions = detect_clock_offsets(inputs);
        const auto repaired = repair_metadata(inputs, params);
        if (st.stop_requested()) {
          return;
        }

        std::vector<store::metadata_update> updates;
        repair_complete_evt complete;
        for (std::size_t i = 0; i < repaired.size(); ++i) {
          const auto &now = repaired[i];
          if (changed(records[i], now)) {
            updates.push_back({.id = now.id,
                               .taken_unix_ms = now.taken_unix_ms,
                               .latitude = now.latitude,
                               .longitude = now.longitude,
                               .date_source = now.date_source,
                               .gps_source = now.gps_source});
          }
          if (changed(records[i], now) || synthetic(now)) {
            queue.push(repair_item_evt{.value = now});
          }
          if (now.date_source == "filename") {
            ++complete.dates_from_filename;
          } else if (now.date_source == "modified") {
            ++complete.dates_from_modified_time;
          } else if (now.date_source == "exif+offset") {
            ++complete.dates_shifted;
          }
          if (now.gps_source == "inferred") {
            ++complete.gps_filled;
          }
        }
        store::update_image_metadata(session_db_, updates);

        for (const auto &suggestion : suggestions) {
          const auto it =
              params.offset_minutes_by_camera.find(suggestion.camera);
          queue.push(repair_offset_evt{
              .value = suggestion,
              .applied = it != params.offset_minutes_by_camera.end() &&
                         it->second == suggestion.offset_minutes});
        }
        queue.push(repair_progress_evt{.done = records.size(),
                                       .total = records.size()});
        queue.push(complete);
      });

  grpc::Status status = stream_pass(
      context, writer, queue, stop_source,
      [&](const repair_event &ev) -> bool {
        RepairEvent proto;
        std::visit(
            [&](const auto &e) -> auto {
              using evt = std::decay_t<decltype(e)>;
              if constexpr (std::is_same_v<evt, repair_progress_evt>) {
                auto *p = proto.mutable_progress();
                p->set_done(static_cast<uint32_t>(e.done));
                p->set_total(static_cast<uint32_t>(e.total));
              } else if constexpr (std::is_same_v<evt, repair_item_evt>) {
                auto *r = proto.mutable_repair();
                r->set_image_id(e.value.id);
                if (e.value.taken_unix_ms.has_value()) {
                  r->set_taken_unix_ms(*e.value.taken_unix_ms);
                }
                if (e.value.latitude.has_value() &&
                    e.value.longitude.has_value()) {
                  auto *gps = r->mutable_gps();
                  gps->set_latitude(*e.value.latitude);
                  gps->set_longitude(*e.value.longitude);
                }
                r->set_date_source(e.value.date_source);
                r->set_gps_source(e.value.gps_source);
              } else if constexpr (std::is_same_v<evt, repair_offset_evt>) {
                auto *o = proto.mutable_offset();
                o->set_camera(e.value.camera);
                o->set_offset_minutes(e.value.offset_minutes);
                o->set_photos(static_cast<uint32_t>(e.value.photos));
                o->set_matched(static_cast<uint32_t>(e.value.matched));
                o->set_matched_unshifted(
                    static_cast<uint32_t>(e.value.matched_unshifted));
                o->set_applied(e.applied);
              } else {
                auto *c = proto.mutable_complete();
                c->set_dates_from_filename(
                    static_cast<uint32_t>(e.dates_from_filename));
                c->set_dates_from_modified_time(
                    static_cast<uint32_t>(e.dates_from_modified_time));
                c->set_dates_shifted(static_cast<uint32_t>(e.dates_shifted));
                c->set_gps_filled(static_cast<uint32_t>(e.gps_filled));
              }
            },
            ev);
        return writer->Write(proto);
      });

  producer.join();
  if (const auto err = producer_error_status(producer_error)) {
    return *err;
  }
  spdlog::info("repair pass finished");
  return status;
}

} // namespace kustavi
