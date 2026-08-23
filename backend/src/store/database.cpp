#include "store/database.h"
#include "paths.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace kustavi {

database::database() = default;

database::~database() { close(); }

void database::open(const std::filesystem::path &folder_path) {
  close();

  // Construct the hidden operational directory structure dynamically
  fs::path cache_dir = config::cache_path(folder_path);
  fs::create_directories(cache_dir);
  fs::create_directories(config::image_cache_path(cache_dir));

  std::string db_path = config::session_db_path(cache_dir).string();

  int rc = sqlite3_open_v2(db_path.c_str(), &db_,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                               SQLITE_OPEN_NOMUTEX,
                           nullptr);
  if (rc != SQLITE_OK) {
    std::string err = sqlite3_errmsg(db_);
    db_ = nullptr;
    throw sqlite_exception("Failed to open Kustavi database session: " + err);
  }

  // Optimize engine parameters for fast write loops (Safe for local application
  // context)
  execute("PRAGMA journal_mode = WAL;");
  execute("PRAGMA synchronous = NORMAL;");
  execute("PRAGMA foreign_keys = ON;");

  initialize_schema();
}

void database::close() {
  if (db_ != nullptr) {
    sqlite3_close_v2(db_);
    db_ = nullptr;
  }
}

void database::execute(const std::string &sql) {
  char *err_msg = nullptr;
  int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg);
  if (rc != SQLITE_OK) {
    std::string err = (err_msg != nullptr) ? err_msg : "Unknown error";
    if (err_msg != nullptr) {
      sqlite3_free(err_msg);
    }
    throw sqlite_exception("SQL Execution failure: " + err);
  }
}

auto database::prepare(const std::string &sql) -> sqlite_statement {
  sqlite3_stmt *stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), static_cast<int>(sql.length()),
                              &stmt, nullptr);
  if (rc != SQLITE_OK) {
    throw sqlite_exception("SQL Preparation statement failed: " +
                           std::string(sqlite3_errmsg(db_)));
  }
  return sqlite_statement(stmt);
}

void database::begin_transaction() { execute("BEGIN TRANSACTION;"); }
void database::commit_transaction() { execute("COMMIT;"); }
void database::rollback_transaction() { execute("ROLLBACK;"); }

// --- Statement Binding Definitions ---

void sqlite_statement::bind_text(int index, const std::string &value) {
  sqlite3_bind_text(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

void sqlite_statement::bind_path(int index,
                                 const std::filesystem::path &value) {
  sqlite3_bind_text(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

void sqlite_statement::bind_int64(int index, int64_t value) {
  sqlite3_bind_int64(stmt_, index, value);
}

void sqlite_statement::bind_int(int index, int value) {
  sqlite3_bind_int(stmt_, index, value);
}

void sqlite_statement::bind_double(int index, double value) {
  sqlite3_bind_double(stmt_, index, value);
}

void sqlite_statement::bind_null(int index) { sqlite3_bind_null(stmt_, index); }

auto sqlite_statement::step() -> int {
  int rc = sqlite3_step(stmt_);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
    throw sqlite_exception("Database evaluation step failure: " +
                           std::string(sqlite3_errstr(rc)));
  }
  return rc;
}

void sqlite_statement::reset() {
  int rc = sqlite3_reset(stmt_);
  if (rc != SQLITE_OK) {
    throw sqlite_exception("Database reset failure: " +
                           std::string(sqlite3_errstr(rc)));
  }
}

void database::initialize_schema() {
  // Structural layout mirrors the agreed database design
  execute(R"(
        CREATE TABLE IF NOT EXISTS session_state (
            key TEXT PRIMARY KEY,
            value TEXT
        );
        CREATE TABLE IF NOT EXISTS images (
            id TEXT PRIMARY KEY,
            absolute_path TEXT NOT NULL,
            file_name TEXT NOT NULL,
            original_width INTEGER NOT NULL,
            original_height INTEGER NOT NULL,
            size_bytes INTEGER NOT NULL,
            taken_unix_ms INTEGER,
            latitude REAL,
            longitude REAL,
            working_image_path TEXT NOT NULL,
            scanned_at INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS junk_flags (
            image_id TEXT PRIMARY KEY,
            is_junk INTEGER NOT NULL CHECK (is_junk IN (0,1)),
            reason TEXT,
            confidence REAL NOT NULL,
            processed_at INTEGER NOT NULL,
            FOREIGN KEY(image_id) REFERENCES images(id) ON DELETE CASCADE
        );
        CREATE TABLE IF NOT EXISTS user_decisions (
            image_id TEXT PRIMARY KEY,
            decision TEXT NOT NULL CHECK (decision IN ('EXPLICIT_KEEP', 'EXPLICIT_DELETE')),
            updated_at INTEGER NOT NULL,
            FOREIGN KEY(image_id) REFERENCES images(id) ON DELETE CASCADE
        );
        CREATE TABLE IF NOT EXISTS quality_flags (
            image_id TEXT PRIMARY KEY,
            laplacian REAL NOT NULL,
            underexposed REAL NOT NULL,
            overexposed REAL NOT NULL,
            processed_at INTEGER NOT NULL,
            FOREIGN KEY(image_id) REFERENCES images(id) ON DELETE CASCADE
        );
        CREATE TABLE IF NOT EXISTS similar_groups (
            group_id INTEGER NOT NULL,
            image_id TEXT NOT NULL,
            keeper_id TEXT NOT NULL,
            score REAL NOT NULL,
            processed_at INTEGER NOT NULL,
            FOREIGN KEY(image_id) REFERENCES images(id) ON DELETE CASCADE
        );
    )");
}

} // namespace kustavi
