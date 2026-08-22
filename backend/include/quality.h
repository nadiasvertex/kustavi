#pragma once

#include <filesystem>
#include <functional>
#include <vector>

namespace kustavi::image {

struct quality_thresholds {
  // Sharpness (Laplacian Variance)
  double blur_threshold = 100.0; //! Lower means blurrier

  // Exposure (Histogram)
  int low_bin_index = 15;   //! Bins below this are considered shadow region
  int high_bin_index = 240; //! Bins above this are considered highlight region
  double cell_passing_score =
      0.70;                  //! A cell passes if less than 70% is clipped
  int min_passing_cells = 2; //! Minimum number of cells that must look ok
  double underexposed_threshold =
      0.30; //! If more than 30% of the image is clipped, it's underexposed
  double overexposed_threshold =
      0.30; //! If more than 30% of the image is clipped, it's overexposed
};

struct local_image_metrics {
  std::filesystem::path path;
  double laplacian_variance = 0.0;
  double underexposed_ratio = 0.0;
  double overexposed_ratio = 0.0;
  bool valid = false;
};

auto find_low_quality_images(
    quality_thresholds thresholds,
    const std::vector<std::filesystem::path> &paths,
    const std::function<void(std::size_t images_analyzed)> &progress_callback)
    -> std::vector<std::filesystem::path>;
} // namespace kustavi::image
