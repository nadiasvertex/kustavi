#include "pass/keeper_signals.h"

#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect/face.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace kustavi::image {

namespace {

// --- tunables -------------------------------------------------------------
// A face smaller than this fraction of the image's short edge is counted but
// not judged for eyes-open / red-eye (too little pixel data to be reliable).
constexpr double k_min_face_frac = 0.05;
// Red-eye: an eye patch is flagged when its mean red channel exceeds this
// multiple of the brighter of green/blue and is itself reasonably bright.
constexpr double k_redeye_ratio = 1.5;
constexpr double k_redeye_min_red = 60.0;
// Eyes-open proxy: an eye ROI counts as "likely closed" only when it is both
// low-contrast and has almost no dark (iris/pupil/lash) pixels.
constexpr double k_closed_max_stddev = 18.0;
constexpr double k_closed_max_dark_frac = 0.03;
// Gray-world cast at which color_balance hits 0.
constexpr double k_cast_full_penalty = 0.15;
// Ignore near-clipped pixels when estimating the cast.
constexpr int k_shadow_clip = 10;
constexpr int k_highlight_clip = 245;

auto normalized_laplacian(const cv::Mat &gray) -> double {
  if (gray.empty()) {
    return 0.0;
  }
  cv::Mat lap;
  cv::Laplacian(gray, lap, CV_64F);
  cv::Scalar mean;
  cv::Scalar stddev;
  cv::meanStdDev(lap, mean, stddev);
  const double variance = stddev[0] * stddev[0];
  return variance / (variance + 100.0);
}

auto color_balance_from_bgr(const cv::Mat &bgr) -> double {
  if (bgr.empty() || bgr.channels() != 3) {
    return 1.0;
  }

  cv::Mat small;
  const double scale = 256.0 / std::max(bgr.cols, bgr.rows);
  if (scale < 1.0) {
    cv::resize(bgr, small, cv::Size(), scale, scale, cv::INTER_AREA);
  } else {
    small = bgr;
  }

  double sum_b = 0.0;
  double sum_g = 0.0;
  double sum_r = 0.0;
  long count = 0;
  for (int y = 0; y < small.rows; ++y) {
    const auto *row = small.ptr<cv::Vec3b>(y);
    for (int x = 0; x < small.cols; ++x) {
      const cv::Vec3b px = row[x];
      const int lo = std::min({px[0], px[1], px[2]});
      const int hi = std::max({px[0], px[1], px[2]});
      if (lo < k_shadow_clip || hi > k_highlight_clip) {
        continue;
      }
      sum_b += px[0];
      sum_g += px[1];
      sum_r += px[2];
      ++count;
    }
  }
  if (count < 100) {
    return 1.0; // not enough mid-tone signal to judge
  }

  const double mean_b = sum_b / static_cast<double>(count);
  const double mean_g = sum_g / static_cast<double>(count);
  const double mean_r = sum_r / static_cast<double>(count);
  const double mean_all = (mean_b + mean_g + mean_r) / 3.0;
  if (mean_all <= 1e-6) {
    return 1.0;
  }
  const double cast = (std::abs(mean_b - mean_all) + std::abs(mean_g - mean_all) +
                       std::abs(mean_r - mean_all)) /
                      (3.0 * mean_all);
  return std::clamp(1.0 - (cast / k_cast_full_penalty), 0.0, 1.0);
}

// Clamp a rect to the image bounds; empty when the clamp leaves nothing.
auto clamp_rect(const cv::Rect &r, const cv::Size &bounds) -> cv::Rect {
  return r & cv::Rect(0, 0, bounds.width, bounds.height);
}

// Returns {evaluable, likely_open} for one eye ROI. `evaluable` is false when
// the patch is empty or too dark/bright to judge; callers then assume open.
auto eye_open_state(const cv::Mat &bgr, cv::Point2f center, int half)
    -> std::pair<bool, bool> {
  const cv::Rect roi = clamp_rect(
      cv::Rect(cvRound(center.x) - half, cvRound(center.y) - half, half * 2,
               half * 2),
      bgr.size());
  if (roi.width < 4 || roi.height < 4) {
    return {false, true};
  }

  cv::Mat gray;
  cv::cvtColor(bgr(roi), gray, cv::COLOR_BGR2GRAY);
  cv::Scalar mean_s;
  cv::Scalar stddev_s;
  cv::meanStdDev(gray, mean_s, stddev_s);
  const double mean = mean_s[0];
  const double stddev = stddev_s[0];
  if (mean < 25.0 || mean > 235.0) {
    return {false, true}; // ROI washed out / in shadow: cannot judge
  }

  const double dark_cutoff = std::max(35.0, 0.45 * mean);
  int dark = 0;
  for (int y = 0; y < gray.rows; ++y) {
    const auto *row = gray.ptr<uchar>(y);
    for (int x = 0; x < gray.cols; ++x) {
      if (row[x] < dark_cutoff) {
        ++dark;
      }
    }
  }
  const double dark_frac =
      static_cast<double>(dark) / static_cast<double>(gray.total());

  const bool likely_closed = stddev < k_closed_max_stddev &&
                             dark_frac < k_closed_max_dark_frac;
  return {true, !likely_closed};
}

} // namespace

struct keeper_analyzer::impl {
  cv::Ptr<cv::FaceDetectorYN> detector;
};

keeper_analyzer::keeper_analyzer() : impl_(std::make_unique<impl>()) {}
keeper_analyzer::keeper_analyzer(keeper_analyzer &&) noexcept = default;
auto keeper_analyzer::operator=(keeper_analyzer &&) noexcept
    -> keeper_analyzer & = default;
keeper_analyzer::~keeper_analyzer() = default;

auto keeper_analyzer::load(const std::filesystem::path &yunet_onnx)
    -> std::expected<keeper_analyzer, std::string> {
  if (yunet_onnx.empty()) {
    return std::unexpected("no face-detection model configured");
  }
  std::error_code ec;
  if (!std::filesystem::exists(yunet_onnx, ec) || ec) {
    return std::unexpected("face-detection model not found: " +
                           yunet_onnx.string());
  }

  keeper_analyzer analyzer;
  try {
    analyzer.impl_->detector = cv::FaceDetectorYN::create(
        yunet_onnx.string(), "", cv::Size(320, 320), 0.7F, 0.3F, 5000);
  } catch (const cv::Exception &e) {
    return std::unexpected(std::string("failed to load face model: ") +
                           e.what());
  }
  if (analyzer.impl_->detector == nullptr) {
    return std::unexpected("failed to construct face detector");
  }
  return analyzer;
}

auto keeper_analyzer::analyze(const std::filesystem::path &image_path)
    -> keeper_metrics {
  keeper_metrics m;

  const cv::Mat bgr = cv::imread(image_path.string(), cv::IMREAD_COLOR);
  if (bgr.empty()) {
    return m; // valid == false
  }
  m.valid = true;
  m.color_balance = color_balance_from_bgr(bgr);

  cv::Mat faces;
  try {
    impl_->detector->setInputSize(bgr.size());
    impl_->detector->detect(bgr, faces);
  } catch (const cv::Exception &e) {
    spdlog::debug("keeper_analyzer: face detect failed on {}: {}",
                  image_path.string(), e.what());
    return m; // color balance only
  }

  m.face_count = faces.rows;
  if (faces.rows == 0) {
    return m;
  }

  const double min_face = k_min_face_frac * std::min(bgr.cols, bgr.rows);

  double best_face_area = -1.0;
  cv::Rect best_face_roi;

  int usable_faces = 0;
  double open_sum = 0.0;
  int sampled_eyes = 0;
  int redeye_eyes = 0;

  for (int i = 0; i < faces.rows; ++i) {
    const float fx = faces.at<float>(i, 0);
    const float fy = faces.at<float>(i, 1);
    const float fw = faces.at<float>(i, 2);
    const float fh = faces.at<float>(i, 3);
    const cv::Rect face_roi = clamp_rect(
        cv::Rect(cvRound(fx), cvRound(fy), cvRound(fw), cvRound(fh)), bgr.size());
    if (face_roi.empty()) {
      continue;
    }

    const auto area = static_cast<double>(face_roi.area());
    if (area > best_face_area) {
      best_face_area = area;
      best_face_roi = face_roi;
    }

    if (fw < min_face || fh < min_face) {
      continue; // counted, but too small to judge eyes
    }
    ++usable_faces;

    const cv::Point2f right_eye(faces.at<float>(i, 4), faces.at<float>(i, 5));
    const cv::Point2f left_eye(faces.at<float>(i, 6), faces.at<float>(i, 7));
    const double interocular = cv::norm(right_eye - left_eye);
    const int eye_half = std::max(
        4, cvRound((interocular > 1.0 ? 0.22 * interocular : 0.18 * fw)));

    int closed = 0;
    for (const auto &eye : {right_eye, left_eye}) {
      const auto [evaluable, open] = eye_open_state(bgr, eye, eye_half);
      if (evaluable && !open) {
        ++closed;
      }
    }
    open_sum += 1.0 - (static_cast<double>(closed) / 2.0);

    // Red-eye: sample a patch around each eye landmark.
    const int patch = std::max(2, cvRound(fw * 0.06));
    for (const auto &eye : {right_eye, left_eye}) {
      const cv::Rect eye_roi = clamp_rect(
          cv::Rect(cvRound(eye.x) - patch, cvRound(eye.y) - patch, patch * 2,
                   patch * 2),
          bgr.size());
      if (eye_roi.empty()) {
        continue;
      }
      const cv::Scalar mean = cv::mean(bgr(eye_roi)); // BGR order
      const double blue = mean[0];
      const double green = mean[1];
      const double red = mean[2];
      ++sampled_eyes;
      if (red > k_redeye_min_red &&
          red > k_redeye_ratio * std::max(green, blue)) {
        ++redeye_eyes;
      }
    }
  }

  if (best_face_area > 0.0) {
    cv::Mat gray;
    cv::cvtColor(bgr(best_face_roi), gray, cv::COLOR_BGR2GRAY);
    m.largest_face_focus = normalized_laplacian(gray);
  }
  if (usable_faces > 0) {
    m.eyes_open_ratio = open_sum / usable_faces;
  }
  if (sampled_eyes > 0) {
    m.redeye_ratio =
        static_cast<double>(redeye_eyes) / static_cast<double>(sampled_eyes);
  }

  return m;
}

auto color_balance_score(const std::filesystem::path &image_path) -> double {
  const cv::Mat bgr = cv::imread(image_path.string(), cv::IMREAD_COLOR);
  return color_balance_from_bgr(bgr);
}

} // namespace kustavi::image
