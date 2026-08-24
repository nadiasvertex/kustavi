#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kustavi {

/** One photo's clustering inputs (from the session index). */
struct trip_member {
  std::string id;
  std::optional<std::int64_t> taken_unix_ms;
  std::optional<double> latitude;
  std::optional<double> longitude;
};

/** A chronological cluster of photos. */
struct trip {
  uint32_t id = 0;
  std::int64_t start_unix_ms = 0;
  std::int64_t end_unix_ms = 0;
  std::vector<std::string> image_ids;       //! Chronological order.
  std::optional<double> centroid_latitude;  //! Mean of members with GPS.
  std::optional<double> centroid_longitude; //! Mean of members with GPS.
  std::optional<std::string> folder;        //! User-visible folder name for grouping.
};

struct trips_result {
  std::vector<trip> trips;
  std::size_t unassigned = 0; //! Images without a usable timestamp.
};

/** Clusters photos into trips chronologically.
 *
 * Two consecutive photos (in time order) belong to the same trip when the
 * time gap is within `max_gap_hours` AND either photo lacks GPS or the
 * haversine distance is within `max_distance_km`. Deterministic for a given
 * input (ties broken by image id).
 */
auto find_trips(const std::vector<trip_member> &members, int max_gap_hours,
                int max_distance_km) -> trips_result;
} // namespace kustavi
