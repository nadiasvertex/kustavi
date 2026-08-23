#include "kustavi_service.h"

#include "algorithm/append_range.h"
#include "pass/trips.h"
#include "paths.h"
#include "store/store.h"

#include <spdlog/spdlog.h>

#include <utility>
#include <vector>

namespace kustavi {

using algorithm::append_range;

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
} // namespace kustavi
