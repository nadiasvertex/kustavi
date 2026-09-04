#pragma once

#include "store/database.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace kustavi::store {

/** One row of the session's image index. */
struct image_record {
  std::string id;
  std::filesystem::path absolute_path;
  std::filesystem::path working_path;
  std::optional<std::int64_t> taken_unix_ms;
  std::optional<double> latitude;
  std::optional<double> longitude;
  std::string kind = "photo"; //! "photo" or "video"
};

/** Get the cached image paths from the database */
auto get_cached_image_paths(database &db) -> std::vector<std::filesystem::path>;

/** Get the original image paths from the database */
auto get_original_image_paths(database &db)
    -> std::vector<std::filesystem::path>;

/** Get every image row (id, paths, EXIF metadata) from the session index. */
auto get_image_records(database &db) -> std::vector<image_record>;

/** Get Laplacian sharpness per image id from the quality pass results. */
auto get_quality_scores(database &db)
    -> std::unordered_map<std::string, double>;

/** Clear all of the data from the database. */
auto reset_session(database &db) -> void;

} // namespace kustavi::store
