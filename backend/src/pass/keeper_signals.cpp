#include "pass/keeper_signals.h"

#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect/face.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace kustavi::image {

namespace {

// --- tunables -------------------------------------------------------------
// A face smaller than this fraction of the image's short edge is counted but
// not judged for eyes-open / red-eye (too little pixel data to be reliable).
constexpr double k_min_face_frac = 0.05;
// YuNet loses recall on multi-megapixel phone photos; detect on a copy whose
// long edge is at most this, then scale detections back to full resolution.
constexpr int k_detect_long_edge = 1024;
// YuNet score threshold. Lower than the library default (0.9) to keep
// slightly off-angle / partially-lit faces in bursts.
constexpr float k_face_score = 0.6F;
// An eye is treated as shut when its openness score falls below this.
constexpr double k_eye_open_threshold = 0.45;
// Red-eye: an eye patch is flagged when its mean red channel exceeds this
// multiple of the brighter of green/blue and is itself reasonably bright.
constexpr double k_redeye_ratio = 1.5;
constexpr double k_redeye_min_red = 60.0;
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
  const double cast =
      (std::abs(mean_b - mean_all) + std::abs(mean_g - mean_all) +
       std::abs(mean_r - mean_all)) /
      (3.0 * mean_all);
  return std::clamp(1.0 - (cast / k_cast_full_penalty), 0.0, 1.0);
}

// Clamp a rect to the image bounds; empty when the clamp leaves nothing.
auto clamp_rect(const cv::Rect &r, const cv::Size &bounds) -> cv::Rect {
  return r & cv::Rect(0, 0, bounds.width, bounds.height);
}

auto keeper_debug_enabled() -> bool {
  const char *v = std::getenv("KUSTAVI_KEEPER_DEBUG");
  return v != nullptr && *v != '\0' && *v != '0';
}

// Openness score in [0, 1] for one eye, cropped from `bgr` around `center`
// with the eye box sized from the interocular distance. Higher == more open.
//
// Key idea: an OPEN eye exposes sclera and iris — desaturated, near-neutral
// pixels that facial skin never produces — while a CLOSED eye is entirely
// warm, saturated eyelid skin plus a thin lash line. So the discriminator is
// the fraction of low-saturation pixels in the window, the bright (sclera)
// share of them, and how many rows they span. These hold up across a burst's
// identical lighting, which is all the similar pass needs.
auto eye_openness(const cv::Mat &bgr, cv::Point2f center, double interocular,
                  bool debug) -> std::pair<bool, double> {
  const int half_w = std::max(6, cvRound(0.20 * interocular));
  const int half_h = std::max(4, cvRound(0.13 * interocular));
  const cv::Rect roi =
      clamp_rect(cv::Rect(cvRound(center.x) - half_w,
                          cvRound(center.y) - half_h, half_w * 2, half_h * 2),
                 bgr.size());
  if (roi.width < 8 || roi.height < 6) {
    return {false, 1.0};
  }

  if (debug) {
    const char *dir = std::getenv("KUSTAVI_EYE_DUMP");
    if (dir != nullptr && *dir != '\0') {
      cv::Mat big;
      cv::resize(bgr(roi), big, cv::Size(), 3, 3, cv::INTER_NEAREST);
      cv::imwrite(std::string(dir) + "/eye_" +
                      std::to_string(cvRound(center.x)) + "_" +
                      std::to_string(cvRound(center.y)) + ".png",
                  big);
    }
  }

  cv::Mat hsv;
  cv::cvtColor(bgr(roi), hsv, cv::COLOR_BGR2HSV);
  cv::Mat blurred;
  cv::GaussianBlur(hsv, blurred, cv::Size(3, 3), 0);

  int lowsat = 0;
  int sclera = 0;
  int lowsat_rows = 0;
  for (int y = 0; y < blurred.rows; ++y) {
    const auto *r = blurred.ptr<cv::Vec3b>(y);
    int row_lowsat = 0;
    for (int x = 0; x < blurred.cols; ++x) {
      const int s = r[x][1];
      const int v = r[x][2];
      if (s < 65 && v > 40) {
        ++lowsat;
        ++row_lowsat;
        if (v > 135) {
          ++sclera;
        }
      }
    }
    if (row_lowsat > blurred.cols / 6) {
      ++lowsat_rows;
    }
  }
  const auto total = static_cast<double>(blurred.total());
  const double lowsat_frac = static_cast<double>(lowsat) / total;
  const double sclera_frac = static_cast<double>(sclera) / total;
  const double lowsat_row_frac =
      static_cast<double>(lowsat_rows) / static_cast<double>(blurred.rows);

  // Cues mapped to ~[0,1] then blended. Calibrated on labelled blink/open
  // bursts: closed eyes ~0.0-0.2, open eyes ~0.7-1.0.
  const double s_low = std::clamp((lowsat_frac - 0.05) / 0.30, 0.0, 1.0);
  const double s_scl = std::clamp((sclera_frac - 0.01) / 0.12, 0.0, 1.0);
  const double s_rows = std::clamp((lowsat_row_frac - 0.10) / 0.45, 0.0, 1.0);
  const double openness =
      std::clamp((0.45 * s_low) + (0.35 * s_scl) + (0.20 * s_rows), 0.0, 1.0);

  if (debug) {
    spdlog::info("    eye@({:.0f},{:.0f}) roi={}x{} lowsat_frac={:.3f} "
                 "sclera_frac={:.3f} lowsat_row_frac={:.3f} -> openness={:.3f}",
                 center.x, center.y, roi.width, roi.height, lowsat_frac,
                 sclera_frac, lowsat_row_frac, openness);
  }
  return {true, openness};
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
        yunet_onnx.string(), "", cv::Size(320, 320), k_face_score, 0.3F, 5000);
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

  const bool debug = keeper_debug_enabled();

  // Detect on a downscaled copy, then map detections back to full resolution.
  const double det_scale =
      std::min(1.0, static_cast<double>(k_detect_long_edge) /
                        std::max(bgr.cols, bgr.rows));
  cv::Mat det_img;
  if (det_scale < 1.0) {
    cv::resize(bgr, det_img, cv::Size(), det_scale, det_scale, cv::INTER_AREA);
  } else {
    det_img = bgr;
  }
  const auto up = static_cast<float>(det_scale < 1.0 ? 1.0 / det_scale : 1.0);

  cv::Mat faces;
  try {
    impl_->detector->setInputSize(det_img.size());
    impl_->detector->detect(det_img, faces);
  } catch (const cv::Exception &e) {
    spdlog::debug("keeper_analyzer: face detect failed on {}: {}",
                  image_path.string(), e.what());
    return m; // color balance only
  }

  m.face_count = faces.rows;
  if (debug) {
    spdlog::info("keeper_analyzer: {} ({}x{}, det {}x{}) faces={}",
                 image_path.string(), bgr.cols, bgr.rows, det_img.cols,
                 det_img.rows, faces.rows);
  }
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
    const float fx = faces.at<float>(i, 0) * up;
    const float fy = faces.at<float>(i, 1) * up;
    const float fw = faces.at<float>(i, 2) * up;
    const float fh = faces.at<float>(i, 3) * up;
    const cv::Rect face_roi =
        clamp_rect(cv::Rect(cvRound(fx), cvRound(fy), cvRound(fw), cvRound(fh)),
                   bgr.size());
    if (face_roi.empty()) {
      continue;
    }

    const auto area = static_cast<double>(face_roi.area());
    if (area > best_face_area) {
      best_face_area = area;
      best_face_roi = face_roi;
    }

    if (fw < min_face || fh < min_face) {
      if (debug) {
        spdlog::info("  face {} {}x{} below min {:.0f}px; not judged", i,
                     cvRound(fw), cvRound(fh), min_face);
      }
      continue; // counted, but too small to judge eyes
    }
    ++usable_faces;

    const cv::Point2f right_eye(faces.at<float>(i, 4) * up,
                                faces.at<float>(i, 5) * up);
    const cv::Point2f left_eye(faces.at<float>(i, 6) * up,
                               faces.at<float>(i, 7) * up);
    const double interocular = std::max(4.0, cv::norm(right_eye - left_eye));

    double face_open = 0.0;
    int face_eyes = 0;
    for (const auto &eye : {right_eye, left_eye}) {
      const auto [evaluable, openness] =
          eye_openness(bgr, eye, interocular, debug);
      if (evaluable) {
        face_open += openness;
        ++face_eyes;
      }
    }
    // Raw per-face openness: mean of the eyes we could score, or "open" (1.0)
    // when neither eye was judgeable. Then a calibration ramp centered on the
    // open/blink decision point so a clear blink lands near 0 and a clearly
    // open face near 1.
    const double raw = face_eyes > 0 ? face_open / face_eyes : 1.0;
    const double face_open_conf =
        face_eyes > 0
            ? std::clamp((raw - (k_eye_open_threshold - 0.18)) / 0.36, 0.0, 1.0)
            : 1.0;
    open_sum += face_open_conf;
    if (debug) {
      spdlog::info("  face {} interocular={:.0f} eyes_scored={} raw={:.3f} "
                   "open_conf={:.3f}{}",
                   i, interocular, face_eyes, raw, face_open_conf,
                   raw < k_eye_open_threshold ? " [BLINK]" : "");
    }

    // Red-eye: sample a patch around each eye landmark.
    const int patch = std::max(2, cvRound(fw * 0.06));
    for (const auto &eye : {right_eye, left_eye}) {
      const cv::Rect eye_roi =
          clamp_rect(cv::Rect(cvRound(eye.x) - patch, cvRound(eye.y) - patch,
                              patch * 2, patch * 2),
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
