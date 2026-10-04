// Assertions for the session database's user_version migrations. Exits
// non-zero on failure. Wired into //backend:database_test.

#include "store/database.h"

#include <cstdio>
#include <filesystem>
#include <string>

namespace {

namespace fs = std::filesystem;

int g_failures = 0;

void check(bool ok, const char *what) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) {
    ++g_failures;
  }
}

auto fresh_dir(const char *name) -> fs::path {
  fs::path dir = fs::temp_directory_path() / name;
  fs::remove_all(dir);
  fs::create_directories(dir / ".kustavi-cache");
  return dir;
}

auto has_column(kustavi::database &db, const char *table, const char *column)
    -> bool {
  auto stmt = db.prepare(std::string("PRAGMA table_info(") + table + ");");
  while (stmt.step() == SQLITE_ROW) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) sqlite C API
    const auto *name =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt.raw(), 1));
    if (name != nullptr && std::string(name) == column) {
      return true;
    }
  }
  return false;
}

} // namespace

int main() {
  using kustavi::database;
  using kustavi::sqlite_exception;

  // Fresh database lands on the latest version.
  {
    auto dir = fresh_dir("kustavi_db_fresh");
    database db;
    db.open(dir);
    check(db.schema_version() == database::kSchemaVersion,
          "fresh database is at latest version");
    check(has_column(db, "images", "kind"), "fresh database has images.kind");
    db.close();
    db.open(dir);
    check(db.schema_version() == database::kSchemaVersion,
          "reopen keeps the version");
  }

  // Unversioned legacy cache (user_version 0, old column set) is upgraded.
  {
    auto dir = fresh_dir("kustavi_db_legacy");
    {
      database db;
      db.open(dir);
      db.execute("PRAGMA user_version = 0;");
      db.execute("ALTER TABLE images DROP COLUMN kind;");
      db.execute("ALTER TABLE quality_flags DROP COLUMN focus_peak;");
      db.execute("ALTER TABLE quality_flags DROP COLUMN reasons;");
    }
    database db;
    db.open(dir);
    check(db.schema_version() == database::kSchemaVersion,
          "legacy database is upgraded");
    check(has_column(db, "images", "kind"), "legacy images.kind added");
    check(has_column(db, "quality_flags", "focus_peak"),
          "legacy quality_flags.focus_peak added");
    check(has_column(db, "quality_flags", "reasons"),
          "legacy quality_flags.reasons added");
  }

  // A database from a newer build is refused.
  {
    auto dir = fresh_dir("kustavi_db_newer");
    {
      database db;
      db.open(dir);
      db.execute("PRAGMA user_version = " +
                 std::to_string(database::kSchemaVersion + 1) + ";");
    }
    database db;
    bool threw = false;
    try {
      db.open(dir);
    } catch (const sqlite_exception &) {
      threw = true;
    }
    check(threw, "newer database version is rejected");
  }

  return g_failures == 0 ? 0 : 1;
}
