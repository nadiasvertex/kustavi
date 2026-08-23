#include "quality.h"
#include "exec/scheduler.h"

#include <opencv2/core.hpp>      // For cv::Mat, cv::Scalar, cv::meanStdDev
#include <opencv2/imgcodecs.hpp> // For cv::imread
#include <opencv2/imgproc.hpp>   // For cv::Laplacian, cv::calcHist
#include <spdlog/spdlog.h>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
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
  static constexpr std::array<float, 2> range{0.0f, 256.0f};
  std::array<const float *, 1> hist_range{range.data()};
  cv::Mat hist;
  cv::calcHist(&region, 1, nullptr, cv::Mat(), hist, 1, &hist_size,
               hist_range.data(), true, false);

  const auto total_pixels = static_cast<double>(region.total());
  double under_exposed_weight = 0.0;
  double over_exposed_weight = 0.0;

  for (int i = 0; i < hist_size; ++i) {
    const float bin_val = hist.at<float>(i);

    if (i <= thresholds.low_bin_index) {
      // Quadratic penalty: highest at 0, tapering off toward the threshold
      // index
      const double severity =
          std::pow((thresholds.low_bin_index - i) /
                       static_cast<double>(thresholds.low_bin_index),
                   2);
      under_exposed_weight += bin_val * severity;
    }
    if (i >= thresholds.high_bin_index) {
      // Quadratic penalty: highest at 255
      const double severity = std::pow((i - thresholds.high_bin_index) /
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
  // score (This saves our low-key and dark background photos from getting
  // flagged)
  if (well_exposed_cells >= thresholds.min_passing_cells) {
    return {0.0, global_over};
  }

  return {global_under, global_over};
}

/**
 * Compute the sharpness and exposure metrics for a single image.
 */
auto compute_metrics(const std::filesystem::path &path,
                     const quality_thresholds &thresholds)
    -> local_image_metrics {
  local_image_metrics metrics{.path = path};

  const cv::Mat img = load_image_stage(path);
  if (img.empty()) {
    return metrics;
  }

  metrics.valid = true;
  auto [under, over] = analyze_exposure(img, thresholds);
  metrics.underexposed_ratio = under;
  metrics.overexposed_ratio = over;
  metrics.laplacian_variance = analyze_blur(img);

  return metrics;
}

auto is_flagged(const local_image_metrics &metrics,
                const quality_thresholds &thresholds) -> bool {
  if (!metrics.valid) {
    return false;
  }
  const bool is_blurry = metrics.laplacian_variance < thresholds.blur_threshold;
  const bool is_underexposed =
      metrics.underexposed_ratio > thresholds.underexposed_threshold;
  const bool is_overexposed =
      metrics.overexposed_ratio > thresholds.overexposed_threshold;

  return is_blurry || is_underexposed || is_overexposed;
}

auto analyze_images(quality_thresholds thresholds,
                    const std::vector<std::filesystem::path> &paths,
                    std::stop_token stop_token,
                    const quality_progress_callback &progress_callback,
                    const quality_result_callback &on_result)
    -> std::vector<local_image_metrics> {
  std::vector<local_image_metrics> results(paths.size());
  std::atomic<std::size_t> analyzed_count{0};

  auto scheduler = exec::make_scheduler();

  // Process the entire batch in bulk parallel chunks
  auto work_pipeline =
      ex::schedule(scheduler) |
      ex::bulk(ex::par, paths.size(), [&](std::size_t index) -> void {
        local_image_metrics metrics;
        if (!stop_token.stop_requested()) {
          metrics = compute_metrics(paths[index], thresholds);
          if (on_result) {
            on_result(metrics);
          }
        } else {
          metrics.path = paths[index];
        }

        const std::size_t current_progress =
            analyzed_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (progress_callback) {
          progress_callback(current_progress, paths.size());
        }

        results[index] = std::move(metrics);
      });

  // Synchronously wait for the bulk pipeline to run to completion across
  // threads
  stdexec::sync_wait(work_pipeline);

  return results;
}

auto find_low_quality_images(
    quality_thresholds thresholds,
    const std::vector<std::filesystem::path> &paths,
    const std::function<void(std::size_t images_analyzed)> &progress_callback)
    -> std::vector<std::filesystem::path> {
  const auto metrics = analyze_images(
      thresholds, paths, std::stop_token{},
      [progress_callback](std::size_t done, std::size_t) -> void {
        if (progress_callback) {
          progress_callback(done);
        }
      },
      nullptr);

  std::vector<std::filesystem::path> low_quality;
  for (const auto &m : metrics) {
    if (is_flagged(m, thresholds)) {
      low_quality.push_back(m.path);
    }
  }
  return low_quality;
}
} // namespace kustavi::image
