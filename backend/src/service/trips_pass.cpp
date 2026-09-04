#include "kustavi_service.h"

#include "algorithm/append_range.h"
#include "geo/place_index.h"
#include "pass/trips.h"
#include "paths.h"
#include "store/store.h"

#include <spdlog/spdlog.h>

#include <optional>
#include <utility>
#include <vector>

namespace kustavi {

using algorithm::append_range;

// ---------------------------------------------------------------------------
// Pass 6: trips
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
  trips_params params;
  if (request->max_gap_hours() > 0) {
    params.max_gap_hours = request->max_gap_hours();
  }
  if (request->max_distance_km() > 0) {
    params.max_distance_km = request->max_distance_km();
  }
  if (request->home_radius_km() > 0) {
    params.home_radius_km = request->home_radius_km();
  }
  if (request->leg_radius_km() > 0) {
    params.leg_radius_km = request->leg_radius_km();
  }

  // Reverse-geocoding table for folder names; optional (falls back to
  // month-year folders when the bundled table is missing).
  std::optional<geo::place_index> places;
  if (const auto geo_path = config::geo_data_path(); !geo_path.empty()) {
    auto loaded = geo::place_index::load(geo_path);
    if (loaded) {
      places = std::move(*loaded);
      spdlog::info("trips: loaded {} places from {}", places->size(),
                   geo_path.string());
    } else {
      spdlog::warn("trips: place table unusable ({}); folders fall back to "
                   "month-year",
                   loaded.error());
    }
  } else {
    spdlog::warn(
        "trips: no place table found; folders fall back to month-year");
  }

  event_queue<trips_event> queue;
  std::stop_source stop_source;
  std::exception_ptr producer_error;

  std::thread producer = run_producer(
      queue, stop_source, producer_error,
      [&](const std::stop_token &st) -> void {
        (void)st;
        const auto result =
            find_trips(members, params, places ? &*places : nullptr);
        queue.push(trips_progress_evt{.done = result.trips.size(),
                                      .total = result.trips.size()});
        for (const auto &trip : result.trips) {
          queue.push(trips_result_evt{.value = trip});
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
                const trip &src = e.value;
                auto *t = proto.mutable_trip();
                t->set_id(src.id);
                t->set_start_unix_ms(src.start_unix_ms);
                t->set_end_unix_ms(src.end_unix_ms);
                append_range(t->mutable_image_ids(), src.image_ids);
                if (src.centroid_latitude.has_value() &&
                    src.centroid_longitude.has_value()) {
                  auto *centroid = t->mutable_centroid();
                  centroid->set_latitude(*src.centroid_latitude);
                  centroid->set_longitude(*src.centroid_longitude);
                }
                if (src.folder.has_value()) {
                  t->set_folder(*src.folder);
                }
                t->set_folder_slug(src.folder_slug);
                t->set_place_name(src.place_name);
                t->set_is_home(src.is_home);
                for (const auto &leg : src.legs) {
                  auto *pleg = t->add_legs();
                  pleg->set_place_name(leg.place_name);
                  pleg->set_slug(leg.slug);
                  append_range(pleg->mutable_image_ids(), leg.image_ids);
                  if (leg.centroid_latitude.has_value() &&
                      leg.centroid_longitude.has_value()) {
                    auto *lc = pleg->mutable_centroid();
                    lc->set_latitude(*leg.centroid_latitude);
                    lc->set_longitude(*leg.centroid_longitude);
                  }
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
} // namespace kustavi
