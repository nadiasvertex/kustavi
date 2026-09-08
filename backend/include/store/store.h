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
  std::string file_name;
  std::int64_t original_width = 0;
  std::int64_t original_height = 0;
  std::int64_t size_bytes = 0;
};

/** One persisted keep/delete choice. */
struct user_decision_row {
  std::string image_id;
  bool remove = false; //! true = EXPLICIT_DELETE, false = EXPLICIT_KEEP
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

/** True when the session already holds a scanned image index. */
auto session_has_index(database &db) -> bool;

/** Read one `session_state` value, or nullopt when the key is absent. */
auto get_session_value(database &db, std::string_view key)
    -> std::optional<std::string>;

/** Upsert one `session_state` key/value pair. */
auto set_session_value(database &db, std::string_view key,
                       std::string_view value) -> void;

/**
 * Record the wizard's current step (a `WizardStep` index) plus a wall-clock
 * timestamp, so a later launch can offer to resume at that step.
 */
auto set_wizard_step(database &db, int step) -> void;

/** The saved wizard step, or 0 when none was recorded. */
auto get_wizard_step(database &db) -> int;

/** Every persisted keep/delete choice. */
auto get_user_decisions(database &db) -> std::vector<user_decision_row>;

/** Replace the whole `user_decisions` table with `rows` (one transaction). */
auto replace_user_decisions(database &db,
                            const std::vector<user_decision_row> &rows) -> void;

} // namespace kustavi::store
