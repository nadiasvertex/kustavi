#include "pass/video.h"
#include "pass/quality.h"
#include "pass/similar.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <vector>

namespace kustavi::image {

namespace {

/** Writes a sampled frame to a scratch JPEG so the (image-path-based) junk
 * classifier can score it; deleted on scope exit. */
class scratch_frame {
public:
  explicit scratch_frame(const cv::Mat &frame) {
    static std::atomic<std::uint64_t> counter{0};
    const auto tag =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("kustavi_video_frame_" + std::to_string(tag) + "_" +
             std::to_string(counter.fetch_add(1)) + ".jpg");
    cv::imwrite(path_.string(), frame);
  }
  ~scratch_frame() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }
  scratch_frame(const scratch_frame &) = delete;
  auto operator=(const scratch_frame &) -> scratch_frame & = delete;

  [[nodiscard]] auto path() const -> const std::filesystem::path & {
    return path_;
  }

private:
  std::filesystem::path path_;
};

auto hamming_distance(std::uint64_t a, std::uint64_t b) -> int {
  return std::popcount(a ^ b);
}

} // namespace

auto analyze_video(const std::filesystem::path &path,
                   const video_thresholds &thresholds,
                   junk_classifier *classifier) -> video_metrics {
  video_metrics metrics{.path = path};

  cv::VideoCapture cap(path.string());
  if (!cap.isOpened()) {
    return metrics; // opened_ok stays false: corrupt/unplayable
  }

  const double fps = cap.get(cv::CAP_PROP_FPS);
  const double frame_count = cap.get(cv::CAP_PROP_FRAME_COUNT);
  if (fps <= 0.0 || frame_count <= 0.0) {
    return metrics; // container lies about its own stream: treat as corrupt
  }

  metrics.opened_ok = true;
  metrics.valid = true;
  metrics.duration_ms = static_cast<std::int64_t>((frame_count / fps) * 1000.0);

  if (metrics.duration_ms < thresholds.min_duration_ms) {
    return metrics; // too short: no need to decode any frames
  }

  const int sample_count = std::max(1, thresholds.sample_frame_count);
  std::vector<double> sharpness;
  std::vector<std::uint64_t> hashes;
  sharpness.reserve(static_cast<std::size_t>(sample_count));
  hashes.reserve(static_cast<std::size_t>(sample_count));

  for (int i = 0; i < sample_count; ++i) {
    const double position =
        frame_count * (static_cast<double>(i) + 0.5) / sample_count;
    cap.set(cv::CAP_PROP_POS_FRAMES, position);
    cv::Mat frame;
    if (!cap.read(frame) || frame.empty()) {
      continue;
    }

    cv::Mat gray;
    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    sharpness.push_back(analyze_blur(gray));
    hashes.push_back(compute_dhash(gray));

    if (classifier != nullptr && metrics.junk_reason.empty() &&
        hashes.size() <= 2) {
      const scratch_frame scratch(frame);
      const auto result = classifier->classify(scratch.path());
      if (result.valid && result.is_junk) {
        metrics.junk_reason = result.reason;
        metrics.junk_confidence = result.confidence;
      }
    }
  }

  if (!sharpness.empty()) {
    double total = 0.0;
    for (const double v : sharpness) {
      total += v;
    }
    metrics.avg_laplacian_variance =
        total / static_cast<double>(sharpness.size());
  }

  if (hashes.size() >= 2) {
    int max_distance = 0;
    for (std::size_t i = 1; i < hashes.size(); ++i) {
      max_distance =
          std::max(max_distance, hamming_distance(hashes[i - 1], hashes[i]));
    }
    metrics.is_static = max_distance < thresholds.motion_hamming_threshold;
  }

  return metrics;
}

auto is_flagged(const video_metrics &metrics,
                const video_thresholds &thresholds)
    -> std::pair<bool, std::string> {
  if (!metrics.valid) {
    return {true, "corrupt"};
  }
  if (metrics.duration_ms < thresholds.min_duration_ms) {
    return {true, "too_short"};
  }
  if (metrics.avg_laplacian_variance > 0.0 &&
      metrics.avg_laplacian_variance < thresholds.blur_threshold) {
    return {true, "blurry"};
  }
  if (metrics.is_static) {
    return {true, "static"};
  }
  if (!metrics.junk_reason.empty()) {
    return {true, metrics.junk_reason};
  }
  return {false, ""};
}

} // namespace kustavi::image
