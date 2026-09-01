#include "geo/place_index.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <numbers>
#include <string_view>
#include <unordered_set>

namespace kustavi::geo {

namespace {

constexpr int k_lon_stride = 512; //! > 360 so (lat_idx, lon_idx) never collide.

auto cell_key(int lat_idx, int lon_idx) -> int {
  return (lat_idx * k_lon_stride) + lon_idx;
}

auto lat_bucket(double latitude) -> int {
  const int idx = static_cast<int>(std::floor(latitude)) + 90;
  return std::clamp(idx, 0, 179);
}

auto lon_bucket(double longitude) -> int {
  int idx = static_cast<int>(std::floor(longitude)) + 180;
  idx %= 360;
  if (idx < 0) {
    idx += 360;
  }
  return idx;
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

auto parse_double(std::string_view text, double &out) -> bool {
  const std::from_chars_result result =
      std::from_chars(text.data(), text.data() + text.size(), out);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

auto parse_int(std::string_view text, long long &out) -> bool {
  return std::from_chars(text.data(), text.data() + text.size(), out).ec ==
         std::errc{};
}

} // namespace

auto place_index::load(const std::filesystem::path &tsv)
    -> std::expected<place_index, std::string> {
  std::ifstream in(tsv);
  if (!in) {
    return std::unexpected("cannot open place table: " + tsv.string());
  }

  place_index index;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    // ascii_name \t lat \t lon \t country \t admin1 \t population
    std::vector<std::string_view> fields;
    fields.reserve(6);
    std::string_view rest{line};
    while (fields.size() < 6) {
      const auto tab = rest.find('\t');
      if (tab == std::string_view::npos) {
        fields.push_back(rest);
        break;
      }
      fields.push_back(rest.substr(0, tab));
      rest = rest.substr(tab + 1);
    }
    if (fields.size() < 4 || fields.front().empty()) {
      continue;
    }

    place p;
    p.city = std::string(fields.at(0));
    if (!parse_double(fields.at(1), p.latitude) ||
        !parse_double(fields.at(2), p.longitude)) {
      continue;
    }
    p.country = std::string(fields.at(3));
    if (fields.size() >= 5) {
      p.admin1 = std::string(fields.at(4));
    }
    if (fields.size() >= 6) {
      long long pop = 0;
      if (parse_int(fields.at(5), pop)) {
        p.population = pop;
      }
    }

    const auto slot = static_cast<std::uint32_t>(index.places_.size());
    index.grid_[cell_key(lat_bucket(p.latitude), lon_bucket(p.longitude))]
        .push_back(slot);
    index.places_.push_back(std::move(p));
  }

  if (index.places_.empty()) {
    return std::unexpected("place table is empty: " + tsv.string());
  }
  return index;
}

auto place_index::nearest(double latitude, double longitude) const
    -> std::optional<place> {
  if (places_.empty()) {
    return std::nullopt;
  }

  const int lat0 = lat_bucket(latitude);
  const int lon0 = lon_bucket(longitude);

  std::optional<std::size_t> best;
  double best_km = 0.0;
  int rings_since_hit = 0;

  for (int radius = 0; radius <= 180; ++radius) {
    for (int dlat = -radius; dlat <= radius; ++dlat) {
      const int lat_idx = lat0 + dlat;
      if (lat_idx < 0 || lat_idx > 179) {
        continue;
      }
      for (int dlon = -radius; dlon <= radius; ++dlon) {
        // Only the perimeter of the current ring is new.
        if (radius > 0 && std::abs(dlat) != radius && std::abs(dlon) != radius) {
          continue;
        }
        int lon_idx = (lon0 + dlon) % 360;
        if (lon_idx < 0) {
          lon_idx += 360;
        }
        const auto it = grid_.find(cell_key(lat_idx, lon_idx));
        if (it == grid_.end()) {
          continue;
        }
        for (const auto slot : it->second) {
          const auto &p = places_[slot];
          const double km =
              haversine_km(latitude, longitude, p.latitude, p.longitude);
          if (!best || km < best_km ||
              (km == best_km && p.population > places_[*best].population)) {
            best = slot;
            best_km = km;
          }
        }
      }
    }

    if (best) {
      if (++rings_since_hit > 2) {
        break;
      }
    }
  }

  if (!best) {
    return std::nullopt;
  }
  return places_[*best];
}

auto folder_label(const place &p) -> std::string {
  static const std::unordered_set<std::string_view> federal = {
      "United States", "Canada",    "Australia", "Brazil",
      "Mexico",        "India",     "Russia",    "China",
      "Germany",       "Indonesia",
  };
  if (!p.admin1.empty() && federal.contains(p.country)) {
    return p.city + ", " + p.admin1 + ", " + p.country;
  }
  if (p.country.empty()) {
    return p.city;
  }
  return p.city + ", " + p.country;
}

auto slugify(std::string_view text) -> std::string {
  std::string out;
  out.reserve(text.size());
  bool pending_sep = false;
  for (const unsigned char ch : text) {
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
        (ch >= '0' && ch <= '9')) {
      if (pending_sep && !out.empty()) {
        out.push_back('-');
      }
      pending_sep = false;
      out.push_back(static_cast<char>(
          (ch >= 'A' && ch <= 'Z') ? (ch - 'A' + 'a') : ch));
    } else {
      pending_sep = true;
    }
  }
  return out;
}

} // namespace kustavi::geo
