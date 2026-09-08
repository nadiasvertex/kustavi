#include "pass/quality.h"
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
 * Sharpness of the most in-focus region (see quality.h). Portrait-mode shots
 * have a deliberately blurred background that sinks the whole-frame variance;
 * scoring tiles and taking a high percentile lets a sharp subject clear the
 * threshold on its own.
 */
auto analyze_blur_peak(const cv::Mat &gray, int grid) -> double {
  if (gray.empty()) {
    return 0.0;
  }
  // Below this a tile carries too few pixels for a meaningful variance.
  constexpr int min_tile = 32;
  if (grid < 2 || gray.cols < min_tile * 2 || gray.rows < min_tile * 2) {
    return analyze_blur(gray);
  }
  // Shrink the grid until tiles are at least min_tile on a side.
  grid = std::min(grid, std::min(gray.cols, gray.rows) / min_tile);
  grid = std::max(grid, 2);

  const int tile_w = gray.cols / grid;
  const int tile_h = gray.rows / grid;
  std::vector<double> tile_scores;
  tile_scores.reserve(static_cast<std::size_t>(grid) * grid);
  for (int r = 0; r < grid; ++r) {
    for (int c = 0; c < grid; ++c) {
      const cv::Rect roi(c * tile_w, r * tile_h, tile_w, tile_h);
      tile_scores.push_back(analyze_blur(gray(roi)));
    }
  }

  // Take the second-sharpest tile: this rejects a lone specular or noisy tile
  // that a plain max would latch onto, while a genuinely in-focus subject
  // spans several tiles at this granularity and still registers.
  std::ranges::sort(tile_scores);
  const std::size_t pick = tile_scores.size() >= 2 ? tile_scores.size() - 2 : 0;
  return tile_scores[pick];
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
  // flagged).
  double under =
      well_exposed_cells >= thresholds.min_passing_cells ? 0.0 : global_under;

  // 4. Overall darkness. The clipping score only catches shadows crushed to
  // black; a frame that is simply too dim (nothing near 0, but a low mean)
  // slips past it, and the well-exposed-cell override then clears it entirely.
  // Fold in how far the mean luminance falls below the acceptable floor so a
  // uniformly dark frame still reads as underexposed.
  const double mean_lum = cv::mean(gray)[0];
  const double darkness_deficit =
      std::clamp((thresholds.min_acceptable_mean - mean_lum) /
                     thresholds.min_acceptable_mean,
                 0.0, 1.0);
  under = std::max(under, darkness_deficit);

  return {under, global_over};
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
  metrics.focus_peak_variance = analyze_blur_peak(img);

  return metrics;
}

auto is_underexposed(const local_image_metrics &metrics,
                     const quality_thresholds &thresholds) -> bool {
  return metrics.valid &&
         metrics.underexposed_ratio > thresholds.underexposed_threshold;
}

auto is_overexposed(const local_image_metrics &metrics,
                    const quality_thresholds &thresholds) -> bool {
  return metrics.valid &&
         metrics.overexposed_ratio > thresholds.overexposed_threshold;
}

auto is_blurry(const local_image_metrics &metrics,
               const quality_thresholds &thresholds) -> bool {
  if (!metrics.valid) {
    return false;
  }
  // Sharpness can't be judged on a badly exposed frame: a dark image has a
  // compressed tonal range (low Laplacian variance even in focus) and a
  // blown-out one has none.
  if (is_underexposed(metrics, thresholds) ||
      is_overexposed(metrics, thresholds)) {
    return false;
  }
  // Compare against the sharpest region, not the whole frame, so an
  // intentionally blurred background can't flag a sharp subject. Require the
  // peak to be both below the absolute threshold and not markedly sharper than
  // the frame overall — the latter is what a portrait-mode subject clears.
  const double peak_ceiling =
      std::max(metrics.laplacian_variance * thresholds.blur_peak_ratio,
               thresholds.blur_peak_floor);
  return metrics.focus_peak_variance < thresholds.blur_threshold &&
         metrics.focus_peak_variance < peak_ceiling;
}

auto is_flagged(const local_image_metrics &metrics,
                const quality_thresholds &thresholds) -> bool {
  return is_blurry(metrics, thresholds) ||
         is_underexposed(metrics, thresholds) ||
         is_overexposed(metrics, thresholds);
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
