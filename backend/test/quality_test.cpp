// Assertions for the quality pass.
//
// Two halves:
//
//  1. Synthetic, in-memory checks of the tile-based peak-sharpness measure
//     (analyze_blur_peak) that keeps iPhone portrait-mode shots — a sharp
//     subject over a deliberately blurred background — from being flagged as
//     blurry.
//
//  2. A real-photo corpus under test/photos/ (path overridable with
//     KUSTAVI_TEST_PHOTOS) run through the same public entry points the RPC
//     pass uses — analyze_images() for blur/exposure and find_similar_images()
//     for near-duplicate grouping — with the expected verdict asserted per
//     file.
//
// Exits non-zero on the first failure. Wired into `just test-quality` via
// //backend:quality_test.

#include "pass/quality.h"
#include "pass/similar.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

auto check(bool ok, const std::string &what) -> bool {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) {
    ++g_failures;
  }
  return ok;
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

auto photos_dir() -> std::filesystem::path {
  if (const char *env = std::getenv("KUSTAVI_TEST_PHOTOS")) {
    return env;
  }
  return "test/photos"; // relative to the repo root
}

// The quality flags that apply to one image. Built from the same exported
// predicates quality_pass.cpp's quality_reasons() uses, so this asserts the
// real classification (is_blurry already suppresses itself on a bad exposure).
struct verdict {
  bool blurry = false;
  bool underexposed = false;
  bool overexposed = false;

  [[nodiscard]] auto clean() const -> bool {
    return !blurry && !underexposed && !overexposed;
  }
  [[nodiscard]] auto str() const -> std::string {
    if (clean()) {
      return "clean";
    }
    std::string s;
    if (blurry) {
      s += "blurry ";
    }
    if (underexposed) {
      s += "underexposed ";
    }
    if (overexposed) {
      s += "overexposed ";
    }
    s.pop_back();
    return s;
  }
  auto operator==(const verdict &o) const -> bool {
    return blurry == o.blurry && underexposed == o.underexposed &&
           overexposed == o.overexposed;
  }
};

auto verdict_of(const kustavi::image::local_image_metrics &m,
                const kustavi::image::quality_thresholds &t) -> verdict {
  return {.blurry = kustavi::image::is_blurry(m, t),
          .underexposed = kustavi::image::is_underexposed(m, t),
          .overexposed = kustavi::image::is_overexposed(m, t)};
}

void run_synthetic_blur_checks() {
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
    check(analyze_blur_peak(sharp) > 1000.0,
          "uniformly sharp frame scores high");
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
}

// Each corpus photo and the verdict the quality pass should reach for it. The
// file names describe the defect (or "normal"/"dupe" for the keepers).
struct corpus_case {
  const char *file;
  verdict expected;
};

constexpr std::array<corpus_case, 10> k_corpus = {{
    {.file = "blurry.jpg", .expected = {.blurry = true}},
    // Portrait mode: sharp rabbit, blurred background. Must NOT be flagged —
    // this is the whole reason analyze_blur_peak exists.
    {.file = "portrait-blur.jpg", .expected = {}},
    {.file = "overexposed.jpg", .expected = {.overexposed = true}},
    {.file = "underexposed.jpg", .expected = {.underexposed = true}},
    {.file = "normal-1.jpg", .expected = {}},
    {.file = "normal-2.jpg", .expected = {}},
    // The near-duplicate burst is fine on quality; it exists for the
    // similarity check below.
    {.file = "dupe-1.jpg", .expected = {}},
    {.file = "dupe-2.jpg", .expected = {}},
    {.file = "dupe-3.jpg", .expected = {}},
    {.file = "dupe-4.jpg", .expected = {}},
}};

auto corpus_paths(const std::filesystem::path &dir)
    -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> paths;
  paths.reserve(k_corpus.size());
  for (const auto &c : k_corpus) {
    paths.push_back(dir / c.file);
  }
  return paths;
}

void run_corpus_quality_checks(const std::filesystem::path &dir) {
  const kustavi::image::quality_thresholds thresholds;
  const auto paths = corpus_paths(dir);

  const auto metrics = kustavi::image::analyze_images(
      thresholds, paths, std::stop_token{}, {}, {});

  auto m_it = metrics.begin();
  for (const auto &c : k_corpus) {
    const auto &m = *m_it++;

    if (!check(m.valid, std::string(c.file) + " decodes")) {
      continue;
    }

    const verdict got = verdict_of(m, thresholds);
    const verdict want = c.expected;
    std::printf("       %-18s peak=%.1f under=%.2f over=%.2f -> %s\n", c.file,
                m.focus_peak_variance, m.underexposed_ratio,
                m.overexposed_ratio, got.str().c_str());
    check(got == want, std::string(c.file) + " classified " + want.str() +
                           " (got " + got.str() + ")");
  }
}

void run_corpus_similarity_checks(const std::filesystem::path &dir) {
  const auto groups = kustavi::image::find_similar_images(
      kustavi::image::default_similarity_radius, corpus_paths(dir), {});

  // The four dupe-*.jpg frames are one burst and should land in a single
  // group; nothing else in the corpus should be grouped with them.
  const auto is_dupe = [](const std::filesystem::path &p) -> bool {
    return p.filename().string().starts_with("dupe-");
  };

  std::size_t dupe_groups = 0;
  std::size_t dupe_members = 0;
  bool foreign_member = false;
  for (const auto &group : groups) {
    if (group.size() < 2) {
      continue; // singletons: not near-duplicates (the RPC pass drops these)
    }
    std::string names;
    for (const auto &p : group) {
      names += p.filename().string() + " ";
    }
    std::printf("       group: %s\n", names.c_str());
    if (!std::ranges::any_of(group, is_dupe)) {
      continue; // some other coincidental pairing — not what this checks
    }
    ++dupe_groups;
    for (const auto &p : group) {
      ++dupe_members;
      if (!is_dupe(p)) {
        foreign_member = true;
      }
    }
  }

  check(dupe_groups == 1,
        "the dupe-*.jpg burst forms exactly one similar group");
  check(dupe_members == 4,
        "all four dupe-*.jpg frames are grouped together (got " +
            std::to_string(dupe_members) + ")");
  check(!foreign_member, "no non-dupe photo is grouped with the burst");
}

} // namespace

auto main() -> int {
  run_synthetic_blur_checks();

  const auto dir = photos_dir();
  if (std::filesystem::is_directory(dir)) {
    std::printf("\n-- corpus: %s --\n", dir.string().c_str());
    run_corpus_quality_checks(dir);
    run_corpus_similarity_checks(dir);
  } else {
    check(false, "photo corpus not found at " + dir.string() +
                     " (set KUSTAVI_TEST_PHOTOS)");
  }

  if (g_failures != 0) {
    std::printf("\n%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("\nall quality-pass checks passed\n");
  return 0;
}
