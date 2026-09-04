#pragma once

#include "pass/junk.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

namespace kustavi::image {

struct video_thresholds {
  std::int64_t min_duration_ms =
      1500; //! Below this, the clip is a "pocket record".
  double blur_threshold =
      100.0; //! Same scale as quality_thresholds::blur_threshold.
  int motion_hamming_threshold = 4; //! dHash distance below this = "no motion".
  int sample_frame_count = 8;       //! Evenly spaced frames sampled per video.
};

struct video_metrics {
  std::filesystem::path path;
  bool opened_ok = false;
  std::int64_t duration_ms = 0;
  double avg_laplacian_variance = 0.0;
  bool is_static = false;
  std::string junk_reason; //! Non-photographic classification, if flagged.
  double junk_confidence = 0.0;
  bool valid = false; //! false when the container could not be opened at all.
};

/**
 * @brief Probes a video for duration, sampled-frame sharpness, and
 * frame-to-frame motion; optionally reuses the photo junk classifier against
 * a couple of sampled frames to catch non-photographic content (e.g. screen
 * recordings).
 *
 * A container that fails to open, or reports a non-positive fps/frame count,
 * is treated as corrupt/unplayable (`opened_ok == false`). A too-short clip
 * short-circuits before any frame is decoded.
 *
 * `classifier` is optional so this stays testable without loading the
 * multi-gigabyte vision model.
 */
auto analyze_video(const std::filesystem::path &path,
                   const video_thresholds &thresholds,
                   junk_classifier *classifier = nullptr) -> video_metrics;

/** True and the short reason bucket ("too_short", "corrupt", "blurry",
 * "static", or a junk category) when the video isn't worth keeping. */
auto is_flagged(const video_metrics &metrics,
                const video_thresholds &thresholds)
    -> std::pair<bool, std::string>;

} // namespace kustavi::image
