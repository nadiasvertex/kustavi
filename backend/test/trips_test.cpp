// Standalone assertions for the trips pass clustering + naming. Exits non-zero
// on the first failure. Wired into `just test-backend` via //backend:trips_test.

#include "geo/place_index.h"
#include "pass/trips.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) {
    ++g_failures;
  }
}

constexpr std::int64_t k_hour_ms = 3600LL * 1000;
constexpr std::int64_t k_day_ms = 24 * k_hour_ms;

kustavi::trip_member member(std::string id, std::int64_t ms,
                            std::optional<double> lat = std::nullopt,
                            std::optional<double> lon = std::nullopt) {
  return {.id = std::move(id),
          .taken_unix_ms = ms,
          .latitude = lat,
          .longitude = lon};
}

// A home cluster near San Francisco, a trip to New York, a trip to London.
void test_home_anchored_segmentation() {
  const double sf_lat = 37.77;
  const double sf_lon = -122.42;
  const double ny_lat = 40.71;
  const double ny_lon = -74.01;
  const double lon_lat = 51.51;
  const double lon_lon = -0.13;

  std::vector<kustavi::trip_member> members;
  std::int64_t t = 0;
  // 20 photos at home over three weeks.
  for (int i = 0; i < 20; ++i) {
    members.push_back(member("home/" + std::to_string(i), t, sf_lat, sf_lon));
    t += k_day_ms;
  }
  // A 5-day New York trip.
  for (int i = 0; i < 8; ++i) {
    members.push_back(member("ny/" + std::to_string(i), t, ny_lat, ny_lon));
    t += 12 * k_hour_ms;
  }
  // Back home for a week.
  for (int i = 0; i < 6; ++i) {
    members.push_back(member("home2/" + std::to_string(i), t, sf_lat, sf_lon));
    t += k_day_ms;
  }
  // A London trip a month later.
  t += 20LL * k_day_ms;
  for (int i = 0; i < 10; ++i) {
    members.push_back(member("lon/" + std::to_string(i), t, lon_lat, lon_lon));
    t += 8 * k_hour_ms;
  }

  kustavi::trips_params params;
  const auto result = kustavi::find_trips(members, params, nullptr);

  int away = 0;
  int home = 0;
  for (const auto &trip : result.trips) {
    (trip.is_home ? home : away) += 1;
  }
  check(away == 2, "two away trips (New York, London)");
  check(home >= 1, "at least one home bucket");

  // The away trips should hold only the away photos.
  for (const auto &trip : result.trips) {
    if (trip.is_home) {
      continue;
    }
    for (const auto &id : trip.image_ids) {
      check(id.rfind("home", 0) != 0, "away trip excludes home photos");
    }
  }
}

// The drift guard must split a cluster glued together by clock skew: two
// far-apart places with interleaved timestamps.
void test_drift_guard() {
  std::vector<kustavi::trip_member> members;
  std::int64_t t = 0;
  for (int i = 0; i < 6; ++i) {
    // Tokyo and Reykjavik, one hour apart — impossible without the guard.
    members.push_back(member("tk/" + std::to_string(i), t, 35.68, 139.69));
    t += k_hour_ms;
    members.push_back(member("rk/" + std::to_string(i), t, 64.15, -21.94));
    t += k_hour_ms;
  }

  kustavi::trips_params params;
  params.home_radius_km = 1; // Force everything "away".
  const auto result = kustavi::find_trips(members, params, nullptr);
  check(result.trips.size() >= 4, "drift guard splits interleaved far places");
}

void test_unassigned_counted() {
  std::vector<kustavi::trip_member> members = {
      member("a", 0, 10.0, 10.0),
      {.id = "no-time", .taken_unix_ms = std::nullopt},
      {.id = "no-time2", .taken_unix_ms = std::nullopt},
  };
  kustavi::trips_params params;
  const auto result = kustavi::find_trips(members, params, nullptr);
  check(result.unassigned == 2, "photos without timestamps are unassigned");
}

void test_no_gps_falls_back_to_month() {
  std::vector<kustavi::trip_member> members;
  std::int64_t t = 1'700'000'000'000; // 2023-11-ish
  for (int i = 0; i < 5; ++i) {
    members.push_back(member("p/" + std::to_string(i), t));
    t += k_hour_ms;
  }
  kustavi::trips_params params;
  const auto result = kustavi::find_trips(members, params, nullptr);
  check(result.trips.size() == 1, "no-GPS photos form one time cluster");
  check(result.trips.empty() ? false
                             : result.trips.front().folder.has_value(),
        "no-GPS trip still gets a month-year folder");
}

void test_place_index() {
  const char *path = std::getenv("KUSTAVI_GEO_DATA");
  if (path == nullptr) {
    std::printf("[SKIP] place_index (set KUSTAVI_GEO_DATA to run)\n");
    return;
  }
  auto index = kustavi::geo::place_index::load(path);
  if (!index) {
    check(false, ("place table loads: " + index.error()).c_str());
    return;
  }
  check(index->size() > 10000, "place table has many rows");

  const auto rome = index->nearest(41.9, 12.5);
  check(rome && rome->country == "Italy", "nearest(41.9,12.5) is in Italy");

  const auto slug = kustavi::geo::slugify("Île-de-France, France");
  check(slug == "le-de-france-france" || slug == "ile-de-france-france",
        "slugify strips punctuation and lowercases");
}

} // namespace

int main() {
  test_home_anchored_segmentation();
  test_drift_guard();
  test_unassigned_counted();
  test_no_gps_falls_back_to_month();
  test_place_index();

  if (g_failures > 0) {
    std::printf("\n%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("\nall checks passed\n");
  return 0;
}
