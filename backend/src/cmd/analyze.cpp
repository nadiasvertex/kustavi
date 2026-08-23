#include "cmd/analyze.h"
#include "pass/quality.h"
#include "pass/similar.h"
#include "store/database.h"
#include "store/store.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <ranges>

namespace kustavi::cmd {
void analyze(const std::filesystem::path &folder_path) {
  spdlog::info("Running diagnostic analysis on data in folder '{}'",
               folder_path.string());

  // Open the database and initialize the schema
  kustavi::database db;
  db.open(folder_path);

  constexpr double similarity_radius = 0.15;

  auto image_paths = store::get_original_image_paths(db);
  auto low_quality_paths = image::find_low_quality_images(
      image::quality_thresholds{}, image_paths,
      [](std::size_t images_analyzed) -> void {
        spdlog::info("Analyzed {} images", images_analyzed);
      });

  spdlog::info("Found {} low quality images", low_quality_paths.size());
  for (const auto &path : low_quality_paths) {
    spdlog::info("Low quality image: {}", path.string());
  }

  auto similar_images = image::find_similar_images(
      similarity_radius, image_paths, [](std::size_t images_analyzed) -> void {
        spdlog::info("Analyzed {} images for similarity", images_analyzed);
      });

  for (const auto &[index, group] :
       std::views::zip(std::views::iota(0), similar_images)) {
    if (group.size() < 2) {
      continue;
    }

    spdlog::info("Group {}:", index);
    for (const auto &similar_path : group) {
      spdlog::info("  Similar image: {}", similar_path.string());
    }
  }
}
} // namespace kustavi::cmd
