#include "init.h"
#include "database.h"

#include <spdlog/spdlog.h>

#include <filesystem>

namespace kustavi::cmd {
void initialize(const std::filesystem::path &folder_path) {
  spdlog::info("Initializing database and workspace in folder '{}'",
               folder_path.string());
  // Open the database and initialize the schema
  kustavi::database db;
  db.open(folder_path);
}
} // namespace kustavi::cmd
