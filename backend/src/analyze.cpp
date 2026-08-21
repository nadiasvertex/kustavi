#include "analyze.h"
#include "database.h"
#include "downscaler.h"

#include <spdlog/spdlog.h>

#include <filesystem>

namespace kustavi::cmd {
void analyze(const std::filesystem::path &folder_path) {
  spdlog::info("Running diagnostic analysis on data in folder '{}'",
               folder_path.string());

  // Open the database and initialize the schema
  kustavi::database db;
  db.open(folder_path);
}
} // namespace kustavi::cmd
