#pragma once

#include "store/store.h"
#include "store/database.h"

#include <ctime>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kustavi::store {

/** Get the cached image paths from the database */
auto get_cached_image_paths(database &db)
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
auto get_original_image_paths(database &db)
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
auto get_image_records(database &db) -> std::vector<image_record> {
  std::vector<image_record> records;

  sqlite_statement stmt = db.prepare(
      "SELECT id, absolute_path, working_image_path, taken_unix_ms, latitude, "
      "longitude, kind, file_name, original_width, original_height, size_bytes "
      "FROM images;");

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
    const auto *kind =
        reinterpret_cast<const char *>(sqlite3_column_text(raw, 6));
    if (kind != nullptr) {
      record.kind = kind;
    }
    const auto *file_name =
        reinterpret_cast<const char *>(sqlite3_column_text(raw, 7));
    if (file_name != nullptr) {
      record.file_name = file_name;
    }
    record.original_width = sqlite3_column_int64(raw, 8);
    record.original_height = sqlite3_column_int64(raw, 9);
    record.size_bytes = sqlite3_column_int64(raw, 10);

    records.push_back(std::move(record));
  }

  return records;
}

/** Get Laplacian sharpness per image id from the quality pass results. */
auto get_quality_scores(database &db)
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

auto reset_session(database &db) -> void {
  db.execute("DELETE FROM images; DELETE FROM junk_flags; DELETE FROM "
             "quality_flags; DELETE FROM similar_groups; DELETE FROM "
             "video_flags; DELETE FROM user_decisions; DELETE FROM "
             "session_state;");
}

auto session_has_index(database &db) -> bool {
  auto stmt = db.prepare("SELECT 1 FROM images LIMIT 1;");
  return stmt.step() == SQLITE_ROW;
}

auto get_session_value(database &db, std::string_view key)
    -> std::optional<std::string> {
  auto stmt = db.prepare("SELECT value FROM session_state WHERE key = ?;");
  stmt.bind_text(1, std::string(key));
  if (stmt.step() != SQLITE_ROW) {
    return std::nullopt;
  }
  const auto *value =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt.raw(), 0));
  if (value == nullptr) {
    return std::nullopt;
  }
  return std::string(value);
}

auto set_session_value(database &db, std::string_view key,
                       std::string_view value) -> void {
  auto stmt = db.prepare(
      "INSERT OR REPLACE INTO session_state (key, value) VALUES (?, ?);");
  stmt.bind_text(1, std::string(key));
  stmt.bind_text(2, std::string(value));
  stmt.step();
}

auto set_wizard_step(database &db, int step) -> void {
  set_session_value(db, "wizard_step", std::to_string(step));
  set_session_value(db, "wizard_step_updated_at",
                    std::to_string(std::time(nullptr)));
}

auto get_wizard_step(database &db) -> int {
  const auto raw = get_session_value(db, "wizard_step");
  if (!raw) {
    return 0;
  }
  try {
    return std::stoi(*raw);
  } catch (const std::exception &) {
    return 0;
  }
}

auto get_user_decisions(database &db) -> std::vector<user_decision_row> {
  std::vector<user_decision_row> rows;
  auto stmt = db.prepare("SELECT image_id, decision FROM user_decisions;");
  while (stmt.step() == SQLITE_ROW) {
    const auto *id =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt.raw(), 0));
    const auto *decision =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt.raw(), 1));
    if (id == nullptr || decision == nullptr) {
      continue;
    }
    rows.push_back({.image_id = id,
                    .remove = std::string_view(decision) == "EXPLICIT_DELETE"});
  }
  return rows;
}

auto replace_user_decisions(database &db,
                            const std::vector<user_decision_row> &rows)
    -> void {
  db.begin_transaction();
  try {
    db.execute("DELETE FROM user_decisions;");
    auto stmt = db.prepare("INSERT OR REPLACE INTO user_decisions (image_id, "
                           "decision, updated_at) "
                           "VALUES (?, ?, strftime('%s','now'));");
    for (const auto &row : rows) {
      stmt.bind_text(1, row.image_id);
      stmt.bind_text(2, row.remove ? "EXPLICIT_DELETE" : "EXPLICIT_KEEP");
      stmt.step();
      stmt.reset();
    }
    db.commit_transaction();
  } catch (...) {
    db.rollback_transaction();
    throw;
  }
}
} // namespace kustavi::store
