#include "quality.h"
#include "exec/scheduler.h"

#include <opencv2/core.hpp>      // For cv::Mat, cv::Scalar, cv::meanStdDev
#include <opencv2/imgcodecs.hpp> // For cv::imread
#include <opencv2/imgproc.hpp>   // For cv::Laplacian, cv::calcHist
#include <spdlog/spdlog.h>
#include <stdexec/execution.hpp>

#include <atomic>
#include <ranges>

namespace kustavi::image {

namespace ex = stdexec;

/**
 * Load an image from the given path and convert it to grayscale.
 */
auto load_image_stage(const std::filesystem::path &path) -> cv::Mat {
  // Read as grayscale since both blur and exposure analysis only require
  // intensity
  spdlog::debug("loading image: {}", path.string());
  return cv::imread(path.string(), cv::IMREAD_GRAYSCALE);
}

// Helper to calculate weighted clipping for a specific image region
auto calculate_clipping_weights(const cv::Mat &region,
                                const quality_thresholds &thresholds)
    -> std::pair<double, double> {

  int hist_size = 256;
  float range[] = {0, 256};
  const float *hist_range[] = {range};
  cv::Mat hist;
  cv::calcHist(&region, 1, nullptr, cv::Mat(), hist, 1, &hist_size, hist_range,
               true, false);

  double total_pixels = region.total();
  double under_exposed_weight = 0.0;
  double over_exposed_weight = 0.0;

  for (int i = 0; i < hist_size; ++i) {
    float bin_val = hist.at<float>(i);

    if (i <= thresholds.low_bin_index) {
      // Quadratic penalty: highest at 0, tapering off toward the threshold
      // index
      double severity =
          std::pow((thresholds.low_bin_index - i) /
                       static_cast<double>(thresholds.low_bin_index),
                   2);
      under_exposed_weight += bin_val * severity;
    }
    if (i >= thresholds.high_bin_index) {
      // Quadratic penalty: highest at 255
      double severity = std::pow((i - thresholds.high_bin_index) /
                                     (255.0 - thresholds.high_bin_index),
                                 2);
      over_exposed_weight += bin_val * severity;
    }
  }

  return {under_exposed_weight / total_pixels,
          over_exposed_weight / total_pixels};
}

/**
 *  Compute Laplacian Variance (Sharpness)
 */
auto analyze_blur(const cv::Mat &gray) -> double {
  if (gray.empty()) {
    return 0.0;
  }
  cv::Mat laplacian;
  cv::Laplacian(gray, laplacian, CV_64F);
  cv::Scalar mean, stddev;
  cv::meanStdDev(laplacian, mean, stddev);
  return stddev[0] * stddev[0]; // Variance = stddev^2
}

/**
 * Compute Histogram (Exposure)
 */
auto analyze_exposure(const cv::Mat &gray, const quality_thresholds &thresholds)
    -> std::pair<double, double> {
  // 1. Calculate global weighted score (gives precise gravity of overall
  // clipping)
  auto [global_under, global_over] =
      calculate_clipping_weights(gray, thresholds);

  // 2. Spatial Grid Analysis (to prevent large backgrounds from drowning out
  // the subject)
  int grid_rows = 3;
  int grid_cols = 3;
  int cell_w = gray.cols / grid_cols;
  int cell_h = gray.rows / grid_rows;

  int well_exposed_cells = 0;

  for (int r = 0; r < grid_rows; ++r) {
    for (int c = 0; c < grid_cols; ++c) {
      // Define local window bounding box
      cv::Rect cell_roi(c * cell_w, r * cell_h, cell_w, cell_h);
      cv::Mat cell = gray(cell_roi);

      auto [cell_under, cell_over] =
          calculate_clipping_weights(cell, thresholds);

      // If this specific region isn't heavily clipped, it has good exposure
      // data
      if (cell_under < thresholds.cell_passing_score &&
          cell_over < thresholds.cell_passing_score) {
        well_exposed_cells++;
      }
    }
  }

  // 3. Contextual Override
  // If enough local zones are well-exposed, ignore a high global underexposure
  // score (This saves your low-key and dark background photos from getting
  // flagged)
  if (well_exposed_cells >= thresholds.min_passing_cells) {
    return {0.0, global_over};
  }

  return {global_under, global_over};
}

/**
 * Determine if an image is low quality based on blur and exposure metrics.
 */
auto is_low_quality(quality_thresholds thresholds,
                    const std::filesystem::path &path) -> bool {
  // Step 1: Load image
  cv::Mat img = load_image_stage(path);
  local_image_metrics metrics{.path = path};

  if (img.empty()) {
    return true;
  }

  // Step 2: Analyze image
  metrics.valid = true;
  auto [under, over] = analyze_exposure(img, thresholds);
  metrics.laplacian_variance = analyze_blur(img);
  metrics.underexposed_ratio = under;
  metrics.overexposed_ratio = over;

  // Step 3: Evaluate criteria & return results
  if (!metrics.valid) {
    return true;
  }

  bool is_blurry = metrics.laplacian_variance < thresholds.blur_threshold;
  bool is_underexposed =
      metrics.underexposed_ratio > thresholds.underexposed_threshold;
  bool is_overexposed =
      metrics.overexposed_ratio > thresholds.overexposed_threshold;

  return is_blurry || is_underexposed || is_overexposed;
}

/**
 * Find low quality images in a batch of image paths.
 */
auto find_low_quality_images(
    quality_thresholds thresholds,
    const std::vector<std::filesystem::path> &paths,
    const std::function<void(std::size_t images_analyzed)> &progress_callback)
    -> std::vector<std::filesystem::path> {
  std::vector<bool> low_quality(paths.size());
  std::atomic<std::size_t> analyzed_count{0};

  auto scheduler = exec::make_scheduler();

  // Process the entire batch in bulk parallel chunks
  auto work_pipeline =
      ex::schedule(scheduler) |
      ex::bulk(ex::par, paths.size(), [&](std::size_t index) -> void {
        low_quality[index] = is_low_quality(thresholds, paths[index]);

        std::size_t current_progress =
            analyzed_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (progress_callback) {
          progress_callback(current_progress);
        }
      });

  // Synchronously wait for the bulk pipeline to run to completion across
  // threads
  stdexec::sync_wait(work_pipeline);

  return std::views::zip(paths, low_quality) |
         std::views::filter(
             [](const auto &pair) { return std::get<1>(pair); }) |
         std::views::elements<0> | std::ranges::to<std::vector>();
}
} // namespace kustavi::image
