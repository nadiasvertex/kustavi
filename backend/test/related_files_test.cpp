// Assertions for related-file discovery and the grouped commit. Exits
// non-zero on failure. Wired into `just test-backend` via
// //backend:related_files_test.

#include "pass/commit.h"
#include "pass/related_files.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace kustavi;

int g_failures = 0;

void check(bool ok, const char *what) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) {
    ++g_failures;
  }
}

void touch(const fs::path &path, const std::string &content = "x") {
  fs::create_directories(path.parent_path());
  std::ofstream(path) << content;
}

auto names(const std::vector<fs::path> &paths) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto &p : paths) {
    out.push_back(p.filename().string());
  }
  return out;
}

auto fresh_dir(const char *name) -> fs::path {
  const auto dir = fs::temp_directory_path() / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

void test_discovery() {
  const auto dir = fresh_dir("kustavi_related_discovery");
  for (const char *f :
       {"IMG_1.jpg", "IMG_1.CR2", "IMG_1.xmp", "IMG_1.jpg.xmp", "IMG_1.mov",
        "IMG_1.png", "IMG_10.jpg", "IMG_10.CR2", "clip.mp4", "clip.mov",
        "clip.xmp"}) {
    touch(dir / f);
  }

  const auto found = names(find_companions(dir / "IMG_1.jpg"));
  const std::vector<std::string> expected = {"IMG_1.CR2", "IMG_1.jpg.xmp",
                                             "IMG_1.mov", "IMG_1.xmp"};
  check(found == expected,
        "jpg collects RAW, Live Photo clip and both sidecar styles");
  check(is_live_photo_motion(dir / "IMG_1.mov"),
        "mov beside a same-stem still is a Live Photo clip");
  check(!is_live_photo_motion(dir / "clip.mov"),
        "mov with no same-stem still is a video item");
  check(names(find_companions(dir / "clip.mp4")) ==
            std::vector<std::string>{"clip.xmp"},
        "a stem-sharing mov is not a companion of another video");
  check(names(find_companions(dir / "IMG_10.jpg")) ==
            std::vector<std::string>{"IMG_10.CR2"},
        "IMG_1 files do not leak into IMG_10");
}

void test_grouped_commit() {
  const auto src = fresh_dir("kustavi_related_src");
  const auto dst = fresh_dir("kustavi_related_dst");
  touch(src / "a" / "IMG_1.jpg", "jpg-one");
  touch(src / "a" / "IMG_1.CR2", "raw-one");
  touch(src / "a" / "IMG_1.xmp", "xmp-one");
  touch(src / "b" / "IMG_1.jpg", "jpg-two-longer");
  touch(src / "b" / "IMG_1.CR2", "raw-two-longer");

  std::stop_source stop;
  // Source-tree layout: the group keeps its relative path.
  auto summary = commit_files(
      src, dst, {{.id = "a/IMG_1.jpg", .path = src / "a" / "IMG_1.jpg"}},
      stop.get_token(), nullptr);
  check(summary.copied == 1 && summary.companions == 2 &&
            summary.errors.empty(),
        "commit copies the jpg with its two companions");
  check(fs::exists(dst / "a" / "IMG_1.CR2") && fs::exists(dst / "a" / "IMG_1.xmp"),
        "companions land beside the primary");

  // Trip layout: a same-name collision suffixes the whole group.
  summary = commit_files(
      src, dst,
      {{.id = "a/IMG_1.jpg", .path = src / "a" / "IMG_1.jpg", .dest_subdir = "trip"},
       {.id = "b/IMG_1.jpg", .path = src / "b" / "IMG_1.jpg", .dest_subdir = "trip"}},
      stop.get_token(), nullptr);
  check(fs::exists(dst / "trip" / "IMG_1.jpg") &&
            fs::exists(dst / "trip" / "IMG_1.CR2") &&
            fs::exists(dst / "trip" / "IMG_1.xmp"),
        "first item keeps its names in the trip folder");
  check(fs::exists(dst / "trip" / "IMG_1-2.jpg") &&
            fs::exists(dst / "trip" / "IMG_1-2.CR2"),
        "colliding item's companions take the same -2 suffix");
  check(summary.copied == 2 && summary.companions == 3,
        "summary counts items and companions separately");

  // Re-running is idempotent.
  summary = commit_files(
      src, dst, {{.id = "a/IMG_1.jpg", .path = src / "a" / "IMG_1.jpg"}},
      stop.get_token(), nullptr);
  check(summary.errors.empty() && summary.companions == 2,
        "re-commit reports no conflicts");
}

} // namespace

int main() {
  test_discovery();
  test_grouped_commit();

  if (g_failures > 0) {
    std::printf("\n%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("\nall checks passed\n");
  return 0;
}
