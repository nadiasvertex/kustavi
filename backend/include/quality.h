#pragma once

#include <filesystem>
#include <functional>
#include <vector>

namespace kustavi::image {

struct quality_thresholds {
  // Sharpness (Laplacian Variance)
  double blur_threshold = 100.0; //! Lower means blurrier

  // Exposure (Histogram)
  double underexposed_threshold =
      0.05;                            //! Max % of pixels allowed in bottom bin
  double overexposed_threshold = 0.05; //! Max % of pixels allowed in top bin
  int low_bin_index = 5;    //! Index defining "dark" pixels (out of 256)
  int high_bin_index = 250; //! Index defining "bright" pixels (out of 256)
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
