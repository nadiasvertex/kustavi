#include "database.h"
#include <filesystem>

namespace fs = std::filesystem;

KustaviDatabase::KustaviDatabase() = default;

KustaviDatabase::~KustaviDatabase() {
    Close();
}

void KustaviDatabase::Open(const std::string& folder_path) {
    Close();

    // Construct the hidden operational directory structure dynamically
    fs::path cache_dir = fs::path(folder_path) / ".kustavi-cache";
    fs::create_directories(cache_dir);
    fs::create_directories(cache_dir / "res768");

    std::string db_path = (cache_dir / "session.db").string();

    int rc = sqlite3_open_v2(db_path.c_str(), &db_,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                            nullptr);
    if (rc != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db_);
        db_ = nullptr;
        throw SQLiteException("Failed to open Kustavi database session: " + err);
    }

    // Optimize engine parameters for fast write loops (Safe for local application context)
    Execute("PRAGMA journal_mode = WAL;");
    Execute("PRAGMA synchronous = NORMAL;");
    Execute("PRAGMA foreign_keys = ON;");

    InitializeSchema();
}

void KustaviDatabase::Close() {
    if (db_) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

void KustaviDatabase::Execute(const std::string& sql) {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string err = err_msg ? err_msg : "Unknown error";
        if (err_msg) sqlite3_free(err_msg);
        throw SQLiteException("SQL Execution failure: " + err);
    }
}

SQLiteStatement KustaviDatabase::Prepare(const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), static_cast<int>(sql.length()), &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw SQLiteException("SQL Preparation statement failed: " + std::string(sqlite3_errmsg(db_)));
    }
    return SQLiteStatement(stmt);
}

void KustaviDatabase::BeginTransaction() { Execute("BEGIN TRANSACTION;"); }
void KustaviDatabase::CommitTransaction() { Execute("COMMIT;"); }
void KustaviDatabase::RollbackTransaction() { Execute("ROLLBACK;"); }

// --- Statement Binding Definitions ---

void SQLiteStatement::BindText(int index, const std::string& value) {
    sqlite3_bind_text(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

void SQLiteStatement::BindInt64(int index, int64_t value) {
    sqlite3_bind_int64(stmt_, index, value);
}

void SQLiteStatement::BindInt(int index, int value) {
    sqlite3_bind_int(stmt_, index, value);
}

void SQLiteStatement::BindDouble(int index, double value) {
    sqlite3_bind_double(stmt_, index, value);
}

void SQLiteStatement::BindNull(int index) {
    sqlite3_bind_null(stmt_, index);
}

int SQLiteStatement::Step() {
    int rc = sqlite3_step(stmt_);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        throw SQLiteException("Database evaluation step failure: " + std::string(sqlite3_errstr(rc)));
    }
    return rc;
}

void KustaviDatabase::InitializeSchema() {
    // Structural layout mirrors the agreed database design
    Execute(R"(
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
    )");
}
