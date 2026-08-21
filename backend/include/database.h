#pragma once

#include <sqlite3.h>
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>
#include <optional>

class SQLiteException : public std::runtime_error {
public:
    explicit SQLiteException(const std::string& message) : std::runtime_error(message) {}
};

class SQLiteStatement {
public:
    explicit SQLiteStatement(sqlite3_stmt* stmt) : stmt_(stmt) {}
    ~SQLiteStatement() { if (stmt_) sqlite3_finalize(stmt_); }

    // Disable copying to enforce strict single-ownership RAII rules
    SQLiteStatement(const SQLiteStatement&) = delete;
    SQLiteStatement& operator=(const SQLiteStatement&) = delete;

    SQLiteStatement(SQLiteStatement&& other) noexcept : stmt_(other.stmt_) { other.stmt_ = nullptr; }
    SQLiteStatement& operator=(SQLiteStatement&& other) noexcept {
        if (this != &other) {
            if (stmt_) sqlite3_finalize(stmt_);
            stmt_ = other.stmt_;
            other.stmt_ = nullptr;
        }
        return *this;
    }

    // Explicit binding helpers
    void BindText(int index, const std::string& value);
    void BindInt64(int index, int64_t value);
    void BindInt(int index, int value);
    void BindDouble(int index, double value);
    void BindNull(int index);

    // Execution steps
    int Step();
    sqlite3_stmt* Raw() { return stmt_; }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

class KustaviDatabase {
public:
    KustaviDatabase();
    ~KustaviDatabase();

    void Open(const std::string& folder_path);
    void Close();

    void Execute(const std::string& sql);
    SQLiteStatement Prepare(const std::string& sql);

    // Transaction Management Controls
    void BeginTransaction();
    void CommitTransaction();
    void RollbackTransaction();

    sqlite3* RawHandle() { return db_; }

private:
    sqlite3* db_ = nullptr;
    void InitializeSchema();
};
