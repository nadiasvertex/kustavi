// Standalone assertions for the keeper-signal analysis used by the similar
// pass (color balance, plus face/eye metrics when the YuNet model resolves).
// Exits non-zero on the first failure. Wired in as //backend:keeper_signals_test.

#include "pass/keeper_signals.h"
#include "paths.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

namespace fs = std::filesystem;

int g_failures = 0;

void check(bool ok, const char *what) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) {
    ++g_failures;
  }
}

// Write a flat BGR image to a scratch PNG and return its path.
fs::path write_flat(const std::string &name, const cv::Scalar &bgr) {
  const fs::path path = fs::temp_directory_path() / name;
  const cv::Mat img(240, 320, CV_8UC3, bgr);
  cv::imwrite(path.string(), img);
  return path;
}

void test_color_balance() {
  const auto neutral = write_flat("ks_neutral.png", {128, 128, 128});
  const auto mild = write_flat("ks_mild_cast.png", {120, 120, 140});
  const auto strong = write_flat("ks_strong_cast.png", {110, 110, 190});

  const double n = kustavi::image::color_balance_score(neutral);
  const double m = kustavi::image::color_balance_score(mild);
  const double s = kustavi::image::color_balance_score(strong);

  std::printf("  color_balance: neutral=%.3f mild=%.3f strong=%.3f\n", n, m, s);
  check(n > 0.99, "neutral image scores ~1.0");
  check(m > 0.30 && m < 0.85, "mild cast lands mid-range");
  check(m < n, "mild cast scores below neutral");
  check(s < 0.20, "strong cast is heavily penalized");
  check(s < m, "stronger cast scores below milder cast");

  const double missing =
      kustavi::image::color_balance_score(fs::path("does-not-exist.png"));
  check(missing == 1.0, "undecodable image yields neutral 1.0");

  fs::remove(neutral);
  fs::remove(mild);
  fs::remove(strong);
}

void test_analyzer_load_and_no_face() {
  auto bad = kustavi::image::keeper_analyzer::load(fs::path("nope.onnx"));
  check(!bad.has_value(), "load() fails for a missing model path");

  const fs::path model = kustavi::config::face_model_path();
  if (model.empty()) {
    std::printf("[SKIP] YuNet model not found in runfiles; "
                "face-dependent checks skipped\n");
    return;
  }

  auto loaded = kustavi::image::keeper_analyzer::load(model);
  check(loaded.has_value(), "load() succeeds for the bundled YuNet model");
  if (!loaded) {
    std::printf("  load error: %s\n", loaded.error().c_str());
    return;
  }

  const auto flat = write_flat("ks_no_face.png", {150, 140, 130});
  const auto metrics = loaded->analyze(flat);
  check(metrics.valid, "analyze() marks a decodable image valid");
  check(metrics.face_count == 0, "no face detected in a flat image");
  check(metrics.eyes_open_ratio == 1.0, "eyes_open_ratio stays neutral (1.0)");
  check(metrics.redeye_ratio == 0.0, "redeye_ratio stays neutral (0.0)");
  check(metrics.largest_face_focus == 0.0, "no face focus without a face");
  check(metrics.color_balance > 0.0 && metrics.color_balance <= 1.0,
        "color_balance is in range");

  const auto missing = loaded->analyze(fs::path("does-not-exist.png"));
  check(!missing.valid, "analyze() marks an undecodable image invalid");

  fs::remove(flat);
}

} // namespace

int main() {
  test_color_balance();
  test_analyzer_load_and_no_face();

  if (g_failures > 0) {
    std::printf("\n%d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("\nall checks passed\n");
  return EXIT_SUCCESS;
}
