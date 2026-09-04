// Standalone assertions for the video pass's duration/blur/motion/corruption
// heuristics. Exits non-zero on the first failure. Wired into `just
// test-video` via //backend:video_test. Fixtures are small synthetic clips
// under test/videos/ (a corrupt one is just a truncated normal.mp4).
//
// The vision junk-classifier reuse is not exercised here (analyze_video's
// `classifier` argument is optional and left null); see junk_pass's own
// coverage for that path.

#include "pass/video.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) {
    ++g_failures;
  }
}

auto videos_dir() -> std::filesystem::path {
  if (const char *env = std::getenv("KUSTAVI_TEST_VIDEOS")) {
    return env;
  }
  return "test/videos"; // relative to the repo root
}

} // namespace

int main() {
  using kustavi::image::analyze_video;
  using kustavi::image::is_flagged;
  using kustavi::image::video_thresholds;

  const auto dir = videos_dir();
  const video_thresholds thresholds;

  {
    const auto metrics = analyze_video(dir / "normal.mp4", thresholds);
    check(metrics.valid, "normal.mp4 opens and probes cleanly");
    check(metrics.duration_ms >= thresholds.min_duration_ms,
          "normal.mp4 is long enough");
    const auto [flagged, reason] = is_flagged(metrics, thresholds);
    check(!flagged, ("normal.mp4 is kept (reason=" + reason + ")").c_str());
  }

  {
    const auto metrics = analyze_video(dir / "short.mp4", thresholds);
    const auto [flagged, reason] = is_flagged(metrics, thresholds);
    check(flagged && reason == "too_short",
          "short.mp4 (< min duration) is flagged too_short");
  }

  {
    const auto metrics = analyze_video(dir / "corrupt.mp4", thresholds);
    check(!metrics.opened_ok, "corrupt.mp4 fails to open");
    const auto [flagged, reason] = is_flagged(metrics, thresholds);
    check(flagged && reason == "corrupt", "corrupt.mp4 is flagged corrupt");
  }

  {
    const auto metrics = analyze_video(dir / "blurry.mp4", thresholds);
    check(metrics.valid, "blurry.mp4 opens and probes cleanly");
    const auto [flagged, reason] = is_flagged(metrics, thresholds);
    check(flagged && reason == "blurry", "blurry.mp4 is flagged blurry");
  }

  {
    const auto metrics = analyze_video(dir / "static.mp4", thresholds);
    check(metrics.valid, "static.mp4 opens and probes cleanly");
    check(metrics.is_static, "static.mp4 has no frame-to-frame motion");
    const auto [flagged, reason] = is_flagged(metrics, thresholds);
    check(flagged && reason == "static", "static.mp4 is flagged static");
  }

  if (g_failures > 0) {
    std::printf("\n%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("\nall checks passed\n");
  return 0;
}
