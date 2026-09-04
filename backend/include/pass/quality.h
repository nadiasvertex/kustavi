#pragma once

#include <opencv2/core.hpp>

#include <filesystem>
#include <functional>
#include <thread>
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

using quality_progress_callback =
    std::function<void(std::size_t done, std::size_t total)>;

using quality_result_callback =
    std::function<void(const local_image_metrics &)>;

/**
 * @brief Computes sharpness and exposure metrics for every image in the
 * batch.
 *
 * `on_result` is called (from scheduler threads) per analyzed image;
 * `progress_callback` reports (done, total) as work completes. Images are
 * skipped once `stop_token` is requested; their metrics stay `valid == false`.
 *
 * @return Metrics in the same order as `paths` (invalid for skipped files).
 */
auto analyze_images(quality_thresholds thresholds,
                    const std::vector<std::filesystem::path> &paths,
                    std::stop_token stop_token,
                    const quality_progress_callback &progress_callback,
                    const quality_result_callback &on_result)
    -> std::vector<local_image_metrics>;

/** True when any quality flag applies to the metrics. */
auto is_flagged(const local_image_metrics &metrics,
                const quality_thresholds &thresholds) -> bool;

/** Laplacian variance (sharpness) of a grayscale image; higher = sharper.
 * Shared with the video pass, which scores sampled frames the same way. */
auto analyze_blur(const cv::Mat &gray) -> double;

/**
 * Find low quality images in a batch of image paths.
 */
auto find_low_quality_images(
    quality_thresholds thresholds,
    const std::vector<std::filesystem::path> &paths,
    const std::function<void(std::size_t images_analyzed)> &progress_callback)
    -> std::vector<std::filesystem::path>;
} // namespace kustavi::image
