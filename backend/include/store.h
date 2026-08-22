#pragma once

#include "database.h"

#include <filesystem>
#include <vector>

namespace kustavi::store {

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

} // namespace kustavi::store
