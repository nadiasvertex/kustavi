#include "analyze.h"
#include "database.h"
#include "quality.h"
#include "store.h"

#include <spdlog/spdlog.h>

#include <filesystem>

namespace kustavi::cmd {
void analyze(const std::filesystem::path &folder_path) {
  spdlog::info("Running diagnostic analysis on data in folder '{}'",
               folder_path.string());

  // Open the database and initialize the schema
  kustavi::database db;
  db.open(folder_path);

  auto cached_paths = store::get_original_image_paths(db);
  auto low_quality_paths = image::find_low_quality_images(
      image::quality_thresholds{}, cached_paths,
      [](std::size_t images_analyzed) -> void {
        spdlog::info("Analyzed {} images", images_analyzed);
      });

  spdlog::info("Found {} low quality images", low_quality_paths.size());
  for (const auto &path : low_quality_paths) {
    spdlog::info("Low quality image: {}", path.string());
  }
}
} // namespace kustavi::cmd
