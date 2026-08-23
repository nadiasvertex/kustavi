#include "cmd/init.h"
#include "pass/downscaler.h"
#include "store/database.h"

#include <spdlog/spdlog.h>

#include <filesystem>

namespace kustavi::cmd {
void initialize(const std::filesystem::path &folder_path) {
  spdlog::info("Initializing database and workspace in folder '{}'",
               folder_path.string());
  // Open the database and initialize the schema
  kustavi::database db;
  db.open(folder_path);

  image::execute_folder_ingestion_pass(
      db, folder_path, true, std::stop_token{},
      [](std::size_t files_seen, std::size_t images_found,
         std::size_t images_prepared) -> void {
        spdlog::info("Ingestion progress: {} files seen, {} images found, {} "
                     "images prepared",
                     files_seen, images_found, images_prepared);
      },
      [](const image::ingestion_result &) -> void {});
}
} // namespace kustavi::cmd
