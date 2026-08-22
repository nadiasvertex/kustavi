#pragma once

#include <filesystem>
#include <sqlite3.h>
#include <stdexcept>
#include <string>

namespace kustavi {

class sqlite_exception : public std::runtime_error {
public:
  explicit sqlite_exception(const std::string &message)
      : std::runtime_error(message) {}
};

class sqlite_statement {
public:
  explicit sqlite_statement(sqlite3_stmt *stmt) : stmt_(stmt) {}
  ~sqlite_statement() {
    if (stmt_)
      sqlite3_finalize(stmt_);
  }

  // Disable copying to enforce strict single-ownership RAII rules
  sqlite_statement(const sqlite_statement &) = delete;
  sqlite_statement &operator=(const sqlite_statement &) = delete;

  sqlite_statement(sqlite_statement &&other) noexcept : stmt_(other.stmt_) {
    other.stmt_ = nullptr;
  }
  sqlite_statement &operator=(sqlite_statement &&other) noexcept {
    if (this != &other) {
      if (stmt_)
        sqlite3_finalize(stmt_);
      stmt_ = other.stmt_;
      other.stmt_ = nullptr;
    }
    return *this;
  }

  // Explicit binding helpers
  void bind_text(int index, const std::string &value);
  void bind_path(int index, const std::filesystem::path &value);
  void bind_int64(int index, int64_t value);
  void bind_int(int index, int value);
  void bind_double(int index, double value);
  void bind_null(int index);

  // Execution steps
  int step();
  void reset();
  sqlite3_stmt *raw() { return stmt_; }

private:
  sqlite3_stmt *stmt_ = nullptr;
};

class database {
public:
  database();
  ~database();

  void open(const std::filesystem::path &folder_path);
  void close();

  void execute(const std::string &sql);
  sqlite_statement prepare(const std::string &sql);

  // Transaction Management Controls
  void begin_transaction();
  void commit_transaction();
  void rollback_transaction();

  sqlite3 *raw_handle() { return db_; }

private:
  sqlite3 *db_ = nullptr;
  void initialize_schema();
};
} // namespace kustavi
