// Standalone assertions for the quality pass's blur heuristic — specifically
// the tile-based peak-sharpness measure (analyze_blur_peak) that keeps iPhone
// portrait-mode shots (sharp subject, deliberately blurred background) from
// being flagged as blurry. Exits non-zero on the first failure. Wired into
// `just test-quality` via //backend:quality_test. No fixtures: the test
// images are synthesised in memory.

#include "pass/quality.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <cstdio>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) {
    ++g_failures;
  }
}

// A fine checkerboard: maximal high-frequency content, so its Laplacian
// variance is large — a stand-in for an in-focus region.
auto checker(int w, int h, int square = 4) -> cv::Mat {
  cv::Mat m(h, w, CV_8UC1, cv::Scalar(0));
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      m.at<unsigned char>(y, x) =
          (((x / square) + (y / square)) % 2) != 0 ? 255 : 0;
    }
  }
  return m;
}

} // namespace

auto main() -> int {
  using kustavi::image::analyze_blur;
  using kustavi::image::analyze_blur_peak;

  // 1. Flat frame: no sharpness anywhere.
  {
    const cv::Mat flat(512, 512, CV_8UC1, cv::Scalar(128));
    check(analyze_blur_peak(flat) < 1.0, "flat frame scores ~0");
  }

  // 2. Uniformly sharp frame scores high.
  {
    const cv::Mat sharp = checker(512, 512);
    check(analyze_blur_peak(sharp) > 1000.0, "uniformly sharp frame scores high");
  }

  // 3. Portrait case: a sharp subject patch over a heavily blurred frame vs.
  //    a genuinely blurry frame. The tiled peak separates the two even though
  //    both have most of their area out of focus.
  {
    cv::Mat portrait;
    cv::GaussianBlur(checker(512, 512), portrait, cv::Size(31, 31), 0);
    checker(192, 192).copyTo(portrait(cv::Rect(128, 128, 192, 192)));

    cv::Mat all_blur;
    cv::GaussianBlur(checker(512, 512), all_blur, cv::Size(31, 31), 0);

    const double portrait_whole = analyze_blur(portrait);
    const double portrait_peak = analyze_blur_peak(portrait);
    const double blur_peak = analyze_blur_peak(all_blur);
    std::printf(
        "       portrait whole=%.2f peak=%.2f | fully-blurred peak=%.2f\n",
        portrait_whole, portrait_peak, blur_peak);

    check(portrait_peak > portrait_whole * 3.0,
          "tiled peak >> whole-frame variance for a local sharp subject");
    check(portrait_peak > blur_peak * 10.0,
          "tiled peak separates a sharp subject from a fully blurred frame");
  }

  // 4. Images too small to tile fall back to the whole-frame measure.
  {
    const cv::Mat tiny = checker(40, 40);
    check(analyze_blur_peak(tiny) == analyze_blur(tiny),
          "small image falls back to whole-frame blur");
  }

  if (g_failures != 0) {
    std::printf("\n%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("\nall quality-pass blur checks passed\n");
  return 0;
}
