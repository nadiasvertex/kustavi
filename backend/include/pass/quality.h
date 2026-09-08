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
  //! A frame is only "blurry" if its sharpest region is also no more than this
  //! multiple of the whole-frame sharpness. A portrait-mode shot has a sharp
  //! subject sitting well above its deliberately blurred surround, so its
  //! peak/whole ratio clears this and it is not flagged.
  double blur_peak_ratio = 3.0;
  //! Floor for the ratio test above, so a near-uniform frame (tiny whole-frame
  //! variance) still needs a genuinely sharp region to count as in focus.
  double blur_peak_floor = 12.0;

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
  //! Mean luminance (0-255) below which a frame reads as underexposed on
  //! overall darkness alone, independent of shadow clipping. The shortfall
  //! below this, as a fraction of it, feeds underexposed_ratio.
  double min_acceptable_mean = 70.0;
};

struct local_image_metrics {
  std::filesystem::path path;
  double laplacian_variance = 0.0;  //! Whole-frame sharpness (kept for record)
  double focus_peak_variance = 0.0; //! Sharpness of the most in-focus region
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

/** True when the frame is underexposed (shadow clipping or overall darkness).
 */
auto is_underexposed(const local_image_metrics &metrics,
                     const quality_thresholds &thresholds) -> bool;

/** True when the frame is overexposed (highlight clipping). */
auto is_overexposed(const local_image_metrics &metrics,
                    const quality_thresholds &thresholds) -> bool;

/** True when the sharpest region is below the blur threshold and not markedly
 * sharper than the frame overall. Judge this only on a well-exposed frame —
 * a too-dark or blown-out image has an unreliable sharpness measure. */
auto is_blurry(const local_image_metrics &metrics,
               const quality_thresholds &thresholds) -> bool;

/** Laplacian variance (sharpness) of a grayscale image; higher = sharper.
 * Shared with the video pass, which scores sampled frames the same way. */
auto analyze_blur(const cv::Mat &gray) -> double;

/** Sharpness of the most in-focus region: splits the frame into a `grid`x`grid`
 * tile mosaic and returns the second-highest per-tile Laplacian variance
 * (dropping the top tile guards against a lone specular/noisy outlier).
 * Unlike the whole-frame measure this survives an intentionally blurred
 * background (iPhone portrait mode), which otherwise drags a sharp subject
 * below the blur threshold. Falls back to `analyze_blur` for images too small
 * to tile. */
auto analyze_blur_peak(const cv::Mat &gray, int grid = 8) -> double;

/**
 * Find low quality images in a batch of image paths.
 */
auto find_low_quality_images(
    quality_thresholds thresholds,
    const std::vector<std::filesystem::path> &paths,
    const std::function<void(std::size_t images_analyzed)> &progress_callback)
    -> std::vector<std::filesystem::path>;
} // namespace kustavi::image
