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
};

/** Get the cached image paths from the database */
inline auto get_cached_image_paths(database &db)
    -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> paths;

  // Use the public prepare API from your database wrapper
  sqlite_statement stmt = db.prepare("SELECT working_image_path FROM images;");

  // Step through the rows using your wrapper's step() function
  while (stmt.step() == SQLITE_ROW) {
    // Extract text from column index 0 via the raw sqlite3_stmt pointer
    const auto *raw_text =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt.raw(), 0));

    if (raw_text != nullptr) {
      paths.emplace_back(raw_text);
    }
  }

  return paths;
}

/** Get the original image paths from the database */
inline auto get_original_image_paths(database &db)
    -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> paths;

  // Use the public prepare API from your database wrapper
  sqlite_statement stmt = db.prepare("SELECT absolute_path FROM images;");

  // Step through the rows using your wrapper's step() function
  while (stmt.step() == SQLITE_ROW) {
    // Extract text from column index 0 via the raw sqlite3_stmt pointer
    const auto *raw_text =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt.raw(), 0));

    if (raw_text != nullptr) {
      paths.emplace_back(raw_text);
    }
  }

  return paths;
}

/** Get every image row (id, paths, EXIF metadata) from the session index. */
inline auto get_image_records(database &db) -> std::vector<image_record> {
  std::vector<image_record> records;

  sqlite_statement stmt = db.prepare(
      "SELECT id, absolute_path, working_image_path, taken_unix_ms, latitude, "
      "longitude FROM images;");

  while (stmt.step() == SQLITE_ROW) {
    image_record record;
    sqlite3_stmt *raw = stmt.raw();

    const auto *id =
        reinterpret_cast<const char *>(sqlite3_column_text(raw, 0));
    if (id != nullptr) {
      record.id = id;
    }
    const auto *absolute =
        reinterpret_cast<const char *>(sqlite3_column_text(raw, 1));
    if (absolute != nullptr) {
      record.absolute_path = absolute;
    }
    const auto *working =
        reinterpret_cast<const char *>(sqlite3_column_text(raw, 2));
    if (working != nullptr) {
      record.working_path = working;
    }
    if (sqlite3_column_type(raw, 3) == SQLITE_INTEGER) {
      record.taken_unix_ms = sqlite3_column_int64(raw, 3);
    }
    if (sqlite3_column_type(raw, 4) == SQLITE_FLOAT) {
      record.latitude = sqlite3_column_double(raw, 4);
    }
    if (sqlite3_column_type(raw, 5) == SQLITE_FLOAT) {
      record.longitude = sqlite3_column_double(raw, 5);
    }

    records.push_back(std::move(record));
  }

  return records;
}

/** Get Laplacian sharpness per image id from the quality pass results. */
inline auto get_quality_scores(database &db)
    -> std::unordered_map<std::string, double> {
  std::unordered_map<std::string, double> scores;

  sqlite_statement stmt = db.prepare("SELECT image_id, laplacian FROM "
                                     "quality_flags;");

  while (stmt.step() == SQLITE_ROW) {
    sqlite3_stmt *raw = stmt.raw();
    const auto *id =
        reinterpret_cast<const char *>(sqlite3_column_text(raw, 0));
    if (id == nullptr) {
      continue;
    }
    scores.emplace(id, sqlite3_column_double(raw, 1));
  }

  return scores;
}

} // namespace kustavi::store
