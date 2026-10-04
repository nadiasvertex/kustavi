#include "pass/repair.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <map>
#include <numbers>
#include <regex>

namespace kustavi {

namespace {

constexpr std::int64_t k_minute_ms = 60'000;
constexpr int k_max_shift_minutes = 14 * 60;
constexpr std::int64_t k_match_window_minutes = 5;
constexpr double k_ambiguous_gps_km = 2.0;

auto local_time_ms(int year, int month, int day, int hour, int minute,
                   int second) -> std::optional<std::int64_t> {
  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = second;
  tm.tm_isdst = -1;
  const std::time_t epoch = std::mktime(&tm);
  if (epoch < 0) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(epoch) * 1000;
}

auto haversine_km(double lat1, double lon1, double lat2, double lon2)
    -> double {
  constexpr double earth_km = 6371.0088;
  const auto rad = [](double deg) -> double {
    return deg * std::numbers::pi / 180.0;
  };
  const double dlat = rad(lat2 - lat1);
  const double dlon = rad(lon2 - lon1);
  const double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
                   std::cos(rad(lat1)) * std::cos(rad(lat2)) *
                       std::sin(dlon / 2) * std::sin(dlon / 2);
  return 2 * earth_km * std::asin(std::sqrt(a));
}

struct dated {
  std::optional<std::int64_t> ms;
  std::string source;
  bool trusted = false; //! Precise enough to borrow GPS by time.
};

} // namespace

auto parse_filename_date(std::string_view file_name)
    -> std::optional<filename_date> {
  // Not preceded by a digit; date; optional time of day. The separators cover
  // IMG_20190704_123456, 2019-07-04 at 12.34.56 and Screenshot_2019-07-04-12-34-56;
  // digits after the seconds (PXL_..._123456789) are fractions and ignored.
  static const std::regex pattern(
      R"((?:^|[^0-9])((?:19|20)[0-9]{2})[-_.]?(0[1-9]|1[0-2])[-_.]?(0[1-9]|[12][0-9]|3[01])(?:(?:[ _T-]|[ _]at[ _])?([01][0-9]|2[0-3])[-_.:]?([0-5][0-9])[-_.:]?([0-5][0-9]|60)[0-9]*)?(?:[^0-9]|$))",
      std::regex::ECMAScript);
  const std::string text(file_name);
  std::smatch m;
  if (!std::regex_search(text, m, pattern)) {
    return std::nullopt;
  }
  const auto num = [&](std::size_t i) -> int {
    return std::atoi(m[i].str().c_str());
  };
  const bool has_time = m[4].matched;
  const auto ms = local_time_ms(num(1), num(2), num(3), has_time ? num(4) : 12,
                                has_time ? num(5) : 0, has_time ? num(6) : 0);
  if (!ms) {
    return std::nullopt;
  }
  return filename_date{.unix_ms = *ms, .has_time = has_time};
}

auto repair_metadata(const std::vector<repair_input> &inputs,
                     const repair_params &params)
    -> std::vector<repaired_metadata> {
  // 1. Dates.
  std::vector<dated> dates(inputs.size());
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const auto &in = inputs[i];
    auto &out = dates[i];
    if (in.exif_taken_ms) {
      out.ms = *in.exif_taken_ms;
      out.source = "exif";
      out.trusted = true;
      if (const auto it = params.offset_minutes_by_camera.find(in.camera);
          it != params.offset_minutes_by_camera.end() && it->second != 0 &&
          !in.camera.empty()) {
        *out.ms += static_cast<std::int64_t>(it->second) * k_minute_ms;
        out.source = "exif+offset";
      }
    } else if (const auto from_name = parse_filename_date(in.file_name)) {
      out.ms = from_name->unix_ms;
      out.source = "filename";
      out.trusted = from_name->has_time;
    } else if (in.modified_ms) {
      out.ms = *in.modified_ms;
      out.source = "modified";
    }
  }

  // 2. Positions. Donors are photos with their own EXIF GPS and a trusted time.
  struct donor {
    std::int64_t ms;
    double lat;
    double lon;
  };
  std::vector<donor> donors;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const auto &in = inputs[i];
    if (in.exif_latitude && in.exif_longitude && dates[i].ms &&
        dates[i].trusted) {
      donors.push_back({*dates[i].ms, *in.exif_latitude, *in.exif_longitude});
    }
  }
  std::ranges::sort(donors, {}, &donor::ms);
  const std::int64_t window_ms = params.gps_window_minutes * k_minute_ms;

  std::vector<repaired_metadata> results;
  results.reserve(inputs.size());
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const auto &in = inputs[i];
    repaired_metadata out{.id = in.id,
                          .taken_unix_ms = dates[i].ms,
                          .date_source = dates[i].source};
    if (in.exif_latitude && in.exif_longitude) {
      out.latitude = in.exif_latitude;
      out.longitude = in.exif_longitude;
      out.gps_source = "exif";
    } else if (dates[i].ms && dates[i].trusted && !donors.empty()) {
      const auto t = *dates[i].ms;
      const auto after = std::ranges::lower_bound(donors, t, {}, &donor::ms);
      const donor *next = after != donors.end() ? &*after : nullptr;
      const donor *prev = after != donors.begin() ? &*(after - 1) : nullptr;
      if (next != nullptr && next->ms - t > window_ms) {
        next = nullptr;
      }
      if (prev != nullptr && t - prev->ms > window_ms) {
        prev = nullptr;
      }
      const donor *pick = nullptr;
      if (prev != nullptr && next != nullptr) {
        if (haversine_km(prev->lat, prev->lon, next->lat, next->lon) <=
            k_ambiguous_gps_km) {
          pick = (t - prev->ms <= next->ms - t) ? prev : next;
        }
      } else {
        pick = prev != nullptr ? prev : next;
      }
      if (pick != nullptr) {
        out.latitude = pick->lat;
        out.longitude = pick->lon;
        out.gps_source = "inferred";
      }
    }
    results.push_back(std::move(out));
  }
  return results;
}

auto detect_clock_offsets(const std::vector<repair_input> &inputs,
                          std::size_t min_photos)
    -> std::vector<clock_offset_suggestion> {
  struct camera_stats {
    std::vector<std::int64_t> minutes; //! Whole minutes since the epoch.
    std::size_t with_gps = 0;
  };
  std::map<std::string, camera_stats> cameras;
  for (const auto &in : inputs) {
    if (in.camera.empty() || !in.exif_taken_ms) {
      continue;
    }
    auto &stats = cameras[in.camera];
    stats.minutes.push_back(*in.exif_taken_ms / k_minute_ms);
    if (in.exif_latitude && in.exif_longitude) {
      ++stats.with_gps;
    }
  }

  std::vector<std::int64_t> reference;
  for (const auto &[name, stats] : cameras) {
    if (stats.with_gps * 2 >= stats.minutes.size()) {
      reference.insert(reference.end(), stats.minutes.begin(),
                       stats.minutes.end());
    }
  }
  if (reference.empty()) {
    return {};
  }
  std::ranges::sort(reference);

  // Presence bitmap over reference minutes, widened by the match window, so
  // each (photo, shift) check is O(1).
  const std::int64_t base = reference.front() - k_match_window_minutes;
  const auto span =
      static_cast<std::size_t>(reference.back() - base + k_match_window_minutes + 1);
  std::vector<bool> present(span, false);
  for (const auto minute : reference) {
    for (std::int64_t d = -k_match_window_minutes; d <= k_match_window_minutes;
         ++d) {
      present[static_cast<std::size_t>(minute + d - base)] = true;
    }
  }
  const auto near_reference = [&](std::int64_t minute) -> bool {
    const auto index = minute - base;
    return index >= 0 && static_cast<std::size_t>(index) < span &&
           present[static_cast<std::size_t>(index)];
  };

  std::vector<clock_offset_suggestion> suggestions;
  for (const auto &[name, stats] : cameras) {
    const auto count = stats.minutes.size();
    if (stats.with_gps * 2 >= count || count < min_photos) {
      continue;
    }
    // Distinct minutes with multiplicity keep the scan cheap for bursts.
    std::map<std::int64_t, std::size_t> weighted;
    for (const auto minute : stats.minutes) {
      ++weighted[minute];
    }
    std::vector<std::size_t> hits(2 * k_max_shift_minutes + 1, 0);
    for (int shift = -k_max_shift_minutes; shift <= k_max_shift_minutes;
         ++shift) {
      std::size_t total = 0;
      for (const auto &[minute, weight] : weighted) {
        if (near_reference(minute + shift)) {
          total += weight;
        }
      }
      hits[static_cast<std::size_t>(shift + k_max_shift_minutes)] = total;
    }

    // Best non-zero shift. Photos match within +-5 minutes, so the best score
    // is a plateau of neighbouring shifts: take its middle, then snap to the
    // nearest quarter hour (time zones) when that scores nearly as well.
    const auto hits_at = [&](int shift) -> std::size_t {
      return hits[static_cast<std::size_t>(shift + k_max_shift_minutes)];
    };
    std::size_t best_hits = 0;
    for (int shift = -k_max_shift_minutes; shift <= k_max_shift_minutes;
         ++shift) {
      if (shift != 0) {
        best_hits = std::max(best_hits, hits_at(shift));
      }
    }
    int best_shift = 0;
    for (int shift = -k_max_shift_minutes; shift <= k_max_shift_minutes;
         ++shift) {
      if (shift == 0 || hits_at(shift) != best_hits) {
        continue;
      }
      int end = shift;
      while (end < k_max_shift_minutes && hits_at(end + 1) == best_hits) {
        ++end;
      }
      best_shift = (shift + end) / 2;
      break;
    }
    if (best_shift != 0) {
      const int snapped = static_cast<int>(std::lround(best_shift / 15.0)) * 15;
      if (snapped != 0 && hits_at(snapped) * 10 >= best_hits * 9) {
        best_shift = snapped;
      }
    }
    best_hits = hits_at(best_shift);
    const auto unshifted = hits[static_cast<std::size_t>(k_max_shift_minutes)];
    auto sorted = hits;
    std::ranges::nth_element(sorted, sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2));
    const auto median = sorted[sorted.size() / 2];

    const auto margin = std::max<std::size_t>(5, count / 4);
    const bool clear_winner =
        best_shift != 0 && best_hits >= 5 && best_hits * 5 >= count * 2 &&
        best_hits >= unshifted + margin && 2 * best_hits >= 3 * median;
    if (clear_winner) {
      suggestions.push_back({.camera = name,
                             .offset_minutes = best_shift,
                             .photos = count,
                             .matched = best_hits,
                             .matched_unshifted = unshifted});
    }
  }
  return suggestions;
}

} // namespace kustavi
