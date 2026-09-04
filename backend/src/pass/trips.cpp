#include "pass/trips.h"

#include "geo/place_index.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <numbers>
#include <string>
#include <unordered_map>
#include <utility>

namespace kustavi {

namespace {

auto month_folder(std::int64_t epoch_ms) -> std::string {
  const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1000);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &seconds);
#else
  gmtime_r(&seconds, &utc);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%B %Y", &utc);
  return std::string(buf);
}

auto month_slug(std::int64_t epoch_ms) -> std::string {
  const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1000);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &seconds);
#else
  gmtime_r(&seconds, &utc);
#endif
  char buf[16];
  std::strftime(buf, sizeof(buf), "%Y-%m", &utc);
  return std::string(buf);
}

auto haversine_km(double lat1, double lon1, double lat2, double lon2)
    -> double {
  constexpr double r_km = 6371.0;
  constexpr double deg2rad = std::numbers::pi / 180.0;

  const double dlat = (lat2 - lat1) * deg2rad;
  const double dlon = (lon2 - lon1) * deg2rad;
  const double a = (std::sin(dlat / 2.0) * std::sin(dlat / 2.0)) +
                   (std::cos(lat1 * deg2rad) * std::cos(lat2 * deg2rad) *
                    std::sin(dlon / 2.0) * std::sin(dlon / 2.0));
  return 2.0 * r_km * std::asin(std::min(1.0, std::sqrt(a)));
}

/** A photo with a resolved timestamp, in chronological order. */
struct stamped {
  const trip_member *member;
  std::int64_t ms;
  bool has_gps;
  double lat;
  double lon;
};

/** Running mean of a set of GPS points. */
struct centroid {
  double lat_sum = 0.0;
  double lon_sum = 0.0;
  std::size_t count = 0;

  void add(double lat, double lon) {
    lat_sum += lat;
    lon_sum += lon;
    ++count;
  }
  [[nodiscard]] auto has() const -> bool { return count > 0; }
  [[nodiscard]] auto lat() const -> double {
    return lat_sum / static_cast<double>(count);
  }
  [[nodiscard]] auto lon() const -> double {
    return lon_sum / static_cast<double>(count);
  }
};

/** Home clusters: dense ~11 km cells of GPS photos that recur across the
 * archive's timeline. A trip is temporally localized (a handful of days in
 * one place); home is where photos keep coming back over weeks and months,
 * so a cell only counts as home when its photos span both a large fraction
 * of the whole timeline and at least a week of wall-clock time. */
auto detect_homes(const std::vector<stamped> &sorted)
    -> std::vector<std::pair<double, double>> {
  // ~0.1 degree ≈ 11 km latitude; good enough to seed home clusters, which
  // are then matched by true distance against `home_radius_km`.
  constexpr double cell_deg = 0.1;
  constexpr std::int64_t min_span_ms = 7LL * 24 * 3600 * 1000;
  constexpr double min_span_ratio = 0.5;

  struct cell {
    centroid c;
    std::int64_t first_ms = 0;
    std::int64_t last_ms = 0;
  };
  std::unordered_map<long long, cell> cells;
  std::size_t gps_total = 0;
  std::int64_t global_first = 0;
  std::int64_t global_last = 0;
  for (const auto &s : sorted) {
    if (!s.has_gps) {
      continue;
    }
    if (gps_total == 0) {
      global_first = s.ms;
    }
    global_last = s.ms;
    ++gps_total;
    const auto key =
        (static_cast<long long>(std::llround(std::floor(s.lat / cell_deg)))
         << 20) ^
        static_cast<long long>(std::llround(std::floor(s.lon / cell_deg)));
    auto &entry = cells[key];
    if (entry.c.count == 0) {
      entry.first_ms = s.ms;
    }
    entry.last_ms = s.ms;
    entry.c.add(s.lat, s.lon);
  }
  if (gps_total == 0) {
    return {};
  }

  const auto global_span = static_cast<double>(
      std::max<std::int64_t>(1, global_last - global_first));
  const double count_threshold =
      std::max(5.0, 0.05 * static_cast<double>(gps_total));

  std::vector<std::pair<double, double>> homes;
  for (const auto &[key, value] : cells) {
    const std::int64_t span = value.last_ms - value.first_ms;
    const bool dense = static_cast<double>(value.c.count) >= count_threshold;
    const bool recurs =
        span >= min_span_ms &&
        static_cast<double>(span) / global_span >= min_span_ratio;
    if (dense && recurs) {
      homes.emplace_back(value.c.lat(), value.c.lon());
    }
  }
  return homes;
}

auto is_away(const stamped &s,
             const std::vector<std::pair<double, double>> &homes,
             double home_radius_km, bool prev_away) -> bool {
  if (!s.has_gps) {
    return prev_away; // Carry the state across GPS gaps.
  }
  if (homes.empty()) {
    return true; // No home detected: every cluster is a trip.
  }
  for (const auto &[hlat, hlon] : homes) {
    if (haversine_km(s.lat, s.lon, hlat, hlon) <= home_radius_km) {
      return false;
    }
  }
  return true;
}

/** Splits one away trip's members into contiguous legs. */
auto build_legs(const std::vector<const stamped *> &members,
                double leg_radius_km, const geo::place_index *places)
    -> std::vector<trip_leg> {
  std::vector<trip_leg> legs;
  centroid current;

  for (const auto *sp : members) {
    const auto &s = *sp;
    const bool jump = s.has_gps && current.has() &&
                      haversine_km(current.lat(), current.lon(), s.lat, s.lon) >
                          leg_radius_km;
    if (legs.empty() || jump) {
      legs.emplace_back();
      current = {};
    }
    if (s.has_gps) {
      current.add(s.lat, s.lon);
      legs.back().centroid_latitude = current.lat();
      legs.back().centroid_longitude = current.lon();
    }
    legs.back().image_ids.push_back(s.member->id);
  }

  for (std::size_t i = 0; i < legs.size(); ++i) {
    auto &leg = legs[i];
    if (leg.centroid_latitude && places != nullptr) {
      if (const auto p = places->nearest(*leg.centroid_latitude,
                                         *leg.centroid_longitude)) {
        leg.place_name = geo::folder_label(*p);
        leg.slug = geo::slugify(p->city);
      }
    }
    if (leg.slug.empty()) {
      leg.slug = "leg-" + std::to_string(i + 1);
    }
  }
  return legs;
}

/** Fills folder / place_name / slug for a finished away trip. */
void name_away_trip(trip &t) {
  const std::string month = month_folder(t.start_unix_ms);
  const std::string ym = month_slug(t.start_unix_ms);

  // Distinct leg cities / countries, in first-seen order.
  std::vector<std::string> cities;
  std::vector<std::string> countries;
  for (const auto &leg : t.legs) {
    if (leg.place_name.empty()) {
      continue;
    }
    const auto comma = leg.place_name.rfind(", ");
    const std::string city =
        leg.place_name.substr(0, leg.place_name.find(", "));
    const std::string country = comma == std::string::npos
                                    ? std::string{}
                                    : leg.place_name.substr(comma + 2);
    if (std::ranges::find(cities, city) == cities.end()) {
      cities.push_back(city);
    }
    if (!country.empty() &&
        std::ranges::find(countries, country) == countries.end()) {
      countries.push_back(country);
    }
  }

  if (cities.empty()) {
    // No geocoded legs: keep the historical month-year folder.
    t.folder = month;
    t.folder_slug = ym;
    return;
  }

  if (cities.size() == 1) {
    t.place_name = t.legs.empty() ? cities.front() : t.legs.front().place_name;
    if (t.place_name.empty()) {
      t.place_name = cities.front();
    }
    t.folder = t.place_name + " · " + month;
    t.folder_slug = geo::slugify(t.place_name) + "-" + ym;
    return;
  }

  std::string summary;
  for (std::size_t i = 0; i < cities.size() && i < 3; ++i) {
    summary += (i == 0 ? "" : ", ") + cities[i];
  }
  if (cities.size() > 3) {
    summary += ", …";
  }

  if (countries.size() == 1) {
    t.place_name = countries.front();
    t.folder = countries.front() + " · " + month + " (" + summary + ")";
    t.folder_slug = geo::slugify(countries.front()) + "-" + ym;
  } else if (!countries.empty()) {
    std::string joined;
    for (std::size_t i = 0; i < countries.size(); ++i) {
      joined += (i == 0 ? "" : "–") + countries[i];
    }
    t.place_name = joined;
    t.folder = joined + " · " + month;
    t.folder_slug = geo::slugify(joined) + "-" + ym;
  } else {
    t.place_name = summary;
    t.folder = summary + " · " + month;
    t.folder_slug = geo::slugify(summary) + "-" + ym;
  }
}

} // namespace

auto find_trips(const std::vector<trip_member> &members,
                const trips_params &params, const geo::place_index *places)
    -> trips_result {
  trips_result result;

  std::vector<stamped> sorted;
  sorted.reserve(members.size());
  for (const auto &member : members) {
    if (!member.taken_unix_ms) {
      continue;
    }
    const bool has_gps = member.latitude && member.longitude;
    sorted.push_back({.member = &member,
                      .ms = *member.taken_unix_ms,
                      .has_gps = has_gps,
                      .lat = has_gps ? *member.latitude : 0.0,
                      .lon = has_gps ? *member.longitude : 0.0});
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

  const auto homes = detect_homes(sorted);
  const std::int64_t gap_limit_ms =
      static_cast<std::int64_t>(params.max_gap_hours) * 3600 * 1000;
  const auto max_distance_km = static_cast<double>(params.max_distance_km);

  std::vector<trip> away_trips;
  // (year*12 + month) -> home trip under construction.
  std::unordered_map<int, trip> home_by_month;
  std::unordered_map<int, centroid> home_centroids;

  std::vector<const stamped *> current_members;
  centroid current_centroid;
  std::int64_t current_end = 0;
  bool has_current = false;
  bool prev_away = true;

  const auto flush_away = [&]() {
    if (!has_current) {
      return;
    }
    trip t;
    for (const auto *sp : current_members) {
      t.image_ids.push_back(sp->member->id);
    }
    t.start_unix_ms = current_members.front()->ms;
    t.end_unix_ms = current_members.back()->ms;
    if (current_centroid.has()) {
      t.centroid_latitude = current_centroid.lat();
      t.centroid_longitude = current_centroid.lon();
    }
    t.legs = build_legs(current_members,
                        static_cast<double>(params.leg_radius_km), places);
    name_away_trip(t);
    away_trips.push_back(std::move(t));
    current_members.clear();
    current_centroid = {};
    has_current = false;
  };

  const auto month_key = [](std::int64_t ms) -> int {
    const std::time_t seconds = static_cast<std::time_t>(ms / 1000);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    return ((utc.tm_year + 1900) * 12) + utc.tm_mon;
  };

  for (const auto &s : sorted) {
    const bool away = is_away(
        s, homes, static_cast<double>(params.home_radius_km), prev_away);
    prev_away = away;

    if (!away) {
      flush_away();
      const int key = month_key(s.ms);
      auto &t = home_by_month[key];
      t.image_ids.push_back(s.member->id);
      if (t.start_unix_ms == 0 || s.ms < t.start_unix_ms) {
        t.start_unix_ms = s.ms;
      }
      t.end_unix_ms = s.ms;
      if (s.has_gps) {
        home_centroids[key].add(s.lat, s.lon);
      }
      continue;
    }

    const bool within_time =
        !has_current || (s.ms - current_end) <= gap_limit_ms;
    const bool within_drift =
        !has_current || !current_centroid.has() || !s.has_gps ||
        haversine_km(current_centroid.lat(), current_centroid.lon(), s.lat,
                     s.lon) <= max_distance_km;

    if (has_current && (!within_time || !within_drift)) {
      flush_away();
    }

    has_current = true;
    current_members.push_back(&s);
    current_end = s.ms;
    if (s.has_gps) {
      current_centroid.add(s.lat, s.lon);
    }
  }
  flush_away();

  // Finalise home trips.
  for (auto &[key, t] : home_by_month) {
    t.is_home = true;
    const auto cit = home_centroids.find(key);
    if (cit != home_centroids.end() && cit->second.has()) {
      t.centroid_latitude = cit->second.lat();
      t.centroid_longitude = cit->second.lon();
    }
    t.folder = "Home · " + month_folder(t.start_unix_ms);
    t.folder_slug = "home-" + month_slug(t.start_unix_ms);
    t.place_name = "Home";
    away_trips.push_back(std::move(t));
  }

  std::ranges::sort(away_trips, [](const trip &a, const trip &b) -> bool {
    if (a.start_unix_ms != b.start_unix_ms) {
      return a.start_unix_ms < b.start_unix_ms;
    }
    return a.image_ids < b.image_ids;
  });

  result.trips.reserve(away_trips.size());
  for (std::size_t i = 0; i < away_trips.size(); ++i) {
    away_trips[i].id = static_cast<uint32_t>(i);
    result.trips.push_back(std::move(away_trips[i]));
  }
  return result;
}

} // namespace kustavi
