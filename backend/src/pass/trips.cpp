#include "pass/trips.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kustavi {

namespace {

auto haversine_km(double lat1, double lon1, double lat2, double lon2)
    -> double {
  constexpr double r_km = 6371.0;
  constexpr double deg2rad = std::numbers::pi / 180.0;

  const double dlat = (lat2 - lat1) * deg2rad;
  const double dlon = (lon2 - lon1) * deg2rad;
  const double a = std::sin(dlat / 2.0) * std::sin(dlat / 2.0) +
                   std::cos(lat1 * deg2rad) * std::cos(lat2 * deg2rad) *
                       std::sin(dlon / 2.0) * std::sin(dlon / 2.0);
  return 2.0 * r_km * std::asin(std::min(1.0, std::sqrt(a)));
}

} // namespace

auto find_trips(const std::vector<trip_member> &members, int max_gap_hours,
                int max_distance_km) -> trips_result {
  trips_result result;

  struct stamped {
    const trip_member *member;
    std::int64_t ms;
  };

  std::vector<stamped> sorted;
  sorted.reserve(members.size());
  for (const auto &member : members) {
    if (member.taken_unix_ms.has_value()) {
      sorted.push_back({.member = &member, .ms = *member.taken_unix_ms});
    }
  }
  result.unassigned = members.size() - sorted.size();
  if (sorted.empty()) {
    return result;
  }

  std::ranges::sort(sorted, [](const stamped &a, const stamped &b) -> bool {
    if (a.ms != b.ms) {
      return a.ms < b.ms;
    }
    return a.member->id < b.member->id;
  });

  const std::int64_t gap_limit_ms =
      static_cast<std::int64_t>(max_gap_hours) * 3600 * 1000;

  struct accumulating_trip {
    trip value;
    double gps_lat_sum = 0.0;
    double gps_lon_sum = 0.0;
    std::size_t gps_count = 0;

    void add(const stamped &s) {
      value.image_ids.push_back(s.member->id);
      if (value.start_unix_ms == 0 || s.ms < value.start_unix_ms) {
        value.start_unix_ms = s.ms;
      }
      value.end_unix_ms = s.ms;
      if (s.member->latitude.has_value() && s.member->longitude.has_value()) {
        gps_lat_sum += *s.member->latitude;
        gps_lon_sum += *s.member->longitude;
        gps_count++;
      }
    }

    void finish() {
      if (gps_count > 0) {
        value.centroid_latitude = gps_lat_sum / static_cast<double>(gps_count);
        value.centroid_longitude = gps_lon_sum / static_cast<double>(gps_count);
      }
    }
  };

  std::vector<trip> trips;
  accumulating_trip current;
  bool has_current = false;
  const stamped *last_in_trip = nullptr;

  const auto open_trip = [&](const stamped &s) -> void {
    current = {};
    current.value.start_unix_ms = s.ms;
    current.add(s);
    has_current = true;
    last_in_trip = &s;
  };

  const auto close_trip = [&]() -> void {
    if (!has_current) {
      return;
    }
    current.finish();
    trips.push_back(std::move(current.value));
    has_current = false;
  };

  for (const auto &s : sorted) {
    if (!has_current) {
      open_trip(s);
      continue;
    }

    const bool within_time = (s.ms - current.value.end_unix_ms) <= gap_limit_ms;
    bool within_distance = true;
    if (last_in_trip != nullptr) {
      const auto *prev = last_in_trip->member;
      const auto *cur = s.member;
      if (prev->latitude.has_value() && prev->longitude.has_value() &&
          cur->latitude.has_value() && cur->longitude.has_value()) {
        within_distance = haversine_km(*prev->latitude, *prev->longitude,
                                       *cur->latitude, *cur->longitude) <=
                          static_cast<double>(max_distance_km);
      }
    }

    if (within_time && within_distance) {
      current.add(s);
      last_in_trip = &s;
    } else {
      close_trip();
      open_trip(s);
    }
  }
  close_trip();

  result.trips.reserve(trips.size());
  for (std::size_t i = 0; i < trips.size(); ++i) {
    trips[i].id = static_cast<uint32_t>(i);
    result.trips.push_back(std::move(trips[i]));
  }

  return result;
}
} // namespace kustavi
