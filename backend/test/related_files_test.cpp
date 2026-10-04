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

void test_merge_and_estimate() {
  const auto src = fresh_dir("kustavi_merge_src");
  const auto lib = fresh_dir("kustavi_merge_lib");
  touch(src / "IMG_1.jpg", "already-in-library");
  touch(src / "IMG_1.xmp", "xmp");
  touch(src / "IMG_2.jpg", "brand-new-photo");
  // Same size as IMG_2 but different bytes: must not count as present.
  touch(src / "IMG_3.jpg", std::string("brand-new-photo").substr(0, 14) + "!");
  touch(lib / "Italy-2019-07" / "IMG_9.jpg", "already-in-library");

  const std::vector<commit_source> sources = {
      {.id = "1", .path = src / "IMG_1.jpg", .dest_subdir = "italy-2019-07"},
      {.id = "2", .path = src / "IMG_2.jpg", .dest_subdir = "italy-2019-07"},
      {.id = "3", .path = src / "IMG_3.jpg", .dest_subdir = "italy-2019-07"},
  };

  const auto plain = estimate_commit(src, lib, sources);
  check(plain.already_present == 0 && plain.new_bytes == plain.total_bytes,
        "without merge nothing counts as present");
  const auto merged = estimate_commit(src, lib, sources, {.merge_existing = true});
  check(merged.already_present == 1,
        "merge estimate finds the identical library file under another name");
  check(merged.total_bytes - merged.new_bytes ==
            std::string("already-in-library").size() + 3,
        "present item's bytes, including its sidecar, are not new");
  check(merged.free_bytes.has_value(), "free space is reported");

  std::stop_source stop;
  const auto summary = commit_files(src, lib, sources, stop.get_token(), nullptr,
                                    {.merge_existing = true});
  check(summary.already_present == 1 && summary.copied == 2 &&
            summary.errors.empty(),
        "merge commit skips the duplicate and copies the rest");
  check(fs::exists(lib / "Italy-2019-07" / "IMG_2.jpg") &&
            fs::exists(lib / "Italy-2019-07" / "IMG_3.jpg"),
        "new items go into the existing folder despite letter case");
  std::vector<std::string> folders;
  for (const auto &entry : fs::directory_iterator(lib)) {
    folders.push_back(entry.path().filename().string());
  }
  check(folders == std::vector<std::string>{"Italy-2019-07"} &&
            !fs::exists(lib / "Italy-2019-07" / "IMG_1.jpg"),
        "no parallel folder is created and the duplicate is not copied");
}

} // namespace

int main() {
  test_discovery();
  test_grouped_commit();
  test_merge_and_estimate();

  if (g_failures > 0) {
    std::printf("\n%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("\nall checks passed\n");
  return 0;
}
