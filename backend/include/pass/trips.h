#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kustavi::geo {
class place_index;
} // namespace kustavi::geo

namespace kustavi {

/** One photo's clustering inputs (from the session index). */
struct trip_member {
  std::string id;
  std::optional<std::int64_t> taken_unix_ms;
  std::optional<double> latitude;
  std::optional<double> longitude;
};

/** A contiguous stay at one place within a trip (Rome, then Florence). */
struct trip_leg {
  std::string place_name;                  //! "Rome, Italy"; empty without geo.
  std::string slug;                        //! Filesystem-safe; trip-unique.
  std::vector<std::string> image_ids;      //! Chronological order.
  std::optional<double> centroid_latitude; //! Mean of members with GPS.
  std::optional<double> centroid_longitude;
};

/** A chronological cluster of photos: an away-from-home trip, or a per-month
 * bucket of at-home photos (`is_home`). */
struct trip {
  uint32_t id = 0;
  std::int64_t start_unix_ms = 0;
  std::int64_t end_unix_ms = 0;
  std::vector<std::string> image_ids;      //! Chronological order.
  std::optional<double> centroid_latitude; //! Mean of members with GPS.
  std::optional<double> centroid_longitude;
  std::optional<std::string> folder; //! User-visible folder name for grouping.
  std::string folder_slug;           //! Filesystem-safe form of `folder`.
  std::string place_name;            //! Dominant place ("Italy"); may be empty.
  std::vector<trip_leg> legs;        //! One entry unless the trip has legs.
  bool is_home = false;              //! Photos taken near a detected home.
};

/** Tunables for `find_trips` (GUI sliders; proto defaults applied upstream). */
struct trips_params {
  int max_gap_hours = 48;    //! Time gap that ends an away trip.
  int max_distance_km = 300; //! Centroid drift that ends an away trip.
  int home_radius_km = 15; //! Distance from a home cluster that counts as away.
  int leg_radius_km = 25;  //! Distance from a leg centroid that starts a leg.
};

struct trips_result {
  std::vector<trip> trips;
  std::size_t unassigned = 0; //! Images without a usable timestamp.
};

/** Clusters photos into home-anchored trips.
 *
 * The densest spatial clusters of GPS photos are taken as "home". Photos
 * away from every home are grouped chronologically into trips: consecutive
 * photos join the current trip while the time gap is within
 * `max_gap_hours` AND the running centroid has not drifted past
 * `max_distance_km` (the drift guard breaks physically impossible clusters
 * from clock skew; ordinary travel does not trip it). Each trip is split
 * into contiguous legs whenever GPS jumps past `leg_radius_km`. At-home
 * photos are bucketed by calendar month into `is_home` trips.
 *
 * When `places` is non-null, trip and leg centroids are reverse-geocoded to
 * "City, Country" folder names; otherwise folders fall back to "Month Year".
 * Deterministic for a given input (ties broken by image id).
 */
auto find_trips(const std::vector<trip_member> &members,
                const trips_params &params, const geo::place_index *places)
    -> trips_result;

} // namespace kustavi
