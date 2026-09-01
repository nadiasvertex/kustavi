#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace kustavi::geo {

/** One populated place from the bundled GeoNames table. */
struct place {
  std::string city;
  std::string admin1;  //! First-order division (state/region); may be empty.
  std::string country; //! Full country name ("Italy").
  double latitude = 0.0;
  double longitude = 0.0;
  std::int64_t population = 0;
};

/**
 * @brief Nearest-populated-place lookup over the bundled GeoNames table.
 *
 * Backs the trips pass: a trip's GPS centroid is resolved to the closest
 * city so the folder can be named "Rome, Italy" instead of a raw lat/long.
 * The table (~70k rows) is held in memory and bucketed on a 1-degree
 * equirectangular grid; a lookup scans the centre cell then widening rings
 * until a candidate is found, so it is fast even far from any city.
 */
class place_index {
public:
  /** Loads the tab-separated table produced by
   * `backend/tools/trim_geonames.py`. Lines that do not parse are skipped;
   * an empty or unreadable file yields an error. */
  [[nodiscard]] static auto load(const std::filesystem::path &tsv)
      -> std::expected<place_index, std::string>;

  /** The closest place to `(latitude, longitude)`, or `nullopt` when the
   * table is empty. */
  [[nodiscard]] auto nearest(double latitude, double longitude) const
      -> std::optional<place>;

  [[nodiscard]] auto size() const -> std::size_t { return places_.size(); }

private:
  std::vector<place> places_;
  std::unordered_map<int, std::vector<std::uint32_t>> grid_;
};

/**
 * @brief Folder label for a place: "City, Country", or "City, Region,
 * Country" when a region disambiguates (e.g. US/CA). Never empty for a
 * populated `place`.
 */
[[nodiscard]] auto folder_label(const place &p) -> std::string;

/** A filesystem-safe slug: ASCII alnum runs joined by '-', lowercased. */
[[nodiscard]] auto slugify(std::string_view text) -> std::string;

} // namespace kustavi::geo
