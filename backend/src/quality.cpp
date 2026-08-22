#include "quality.h"
#include "exec/scheduler.h"

#include <opencv2/core.hpp>      // For cv::Mat, cv::Scalar, cv::meanStdDev
#include <opencv2/imgcodecs.hpp> // For cv::imread
#include <opencv2/imgproc.hpp>   // For cv::Laplacian, cv::calcHist
#include <spdlog/spdlog.h>
#include <stdexec/execution.hpp>

#include <algorithm>
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
  if (gray.empty()) {
    return {0.0, 0.0};
  }

  int hist_size = 256;
  float range[] = {0, 256};
  const float *hist_range[] = {range};
  cv::Mat hist;

  cv::calcHist(&gray, 1, nullptr, cv::Mat(), hist, 1, &hist_size, hist_range,
               true, false);

  double total_pixels = gray.total();
  double low_pixels = 0.0;
  double high_pixels = 0.0;

  for (int i = 0; i < hist_size; ++i) {
    float bin_val = hist.at<float>(i);
    if (i <= thresholds.low_bin_index) {
      low_pixels += bin_val;
    }
    if (i >= thresholds.high_bin_index) {
      high_pixels += bin_val;
    }
  }

  return {low_pixels / total_pixels, high_pixels / total_pixels};
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
  std::vector<std::filesystem::path> low_quality_paths;
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

  auto low_quality_views =
      std::views::zip(paths, low_quality) |
      std::views::filter([](const auto &pair) { return std::get<1>(pair); }) |
      std::views::elements<0>;

  std::ranges::copy(low_quality_views, std::back_inserter(low_quality_paths));
  return low_quality_paths;
}
} // namespace kustavi::image
