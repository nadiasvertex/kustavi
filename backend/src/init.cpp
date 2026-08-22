#include "init.h"
#include "database.h"
#include "downscaler.h"

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
      db, folder_path,
      [](int files_seen, int images_found, int images_prepared) -> void {
        spdlog::info("Ingestion progress: {} files seen, {} images found, {} "
                     "images prepared",
                     files_seen, images_found, images_prepared);
      });
}
} // namespace kustavi::cmd
