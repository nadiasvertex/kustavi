#include "pass/related_files.h"

#include "pass/downscaler.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;

namespace kustavi {

namespace {

auto lower(std::string text) -> std::string {
  std::ranges::transform(text, text.begin(), [](unsigned char c) -> char {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

auto lower_extension(const fs::path &path) -> std::string {
  auto ext = path.extension().string();
  if (ext.starts_with(".")) {
    ext.erase(0, 1);
  }
  return lower(std::move(ext));
}

auto is_still_extension(std::string_view ext) -> bool {
  return std::ranges::contains(image::supported_image_extensions, ext);
}

} // namespace

auto is_live_photo_motion(const fs::path &path) -> bool {
  if (lower_extension(path) != "mov") {
    return false;
  }
  const auto stem = lower(path.stem().string());
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(path.parent_path(), ec)) {
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    const auto &sibling = entry.path();
    if (lower(sibling.stem().string()) == stem &&
        is_still_extension(lower_extension(sibling))) {
      return true;
    }
  }
  return false;
}

auto find_companions(const fs::path &primary) -> std::vector<fs::path> {
  std::vector<fs::path> found;
  const auto stem = lower(primary.stem().string());
  const auto full_name = lower(primary.filename().string());
  const auto primary_ext = lower_extension(primary);

  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(primary.parent_path(), ec)) {
    std::error_code type_ec;
    if (!entry.is_regular_file(type_ec) || entry.path() == primary) {
      continue;
    }
    const auto &sibling = entry.path();
    const auto name = lower(sibling.filename().string());
    const auto ext = lower_extension(sibling);

    const bool sidecar_of_full_name =
        ext != primary_ext && name.starts_with(full_name + ".") &&
        std::ranges::contains(companion_extensions, ext);
    const bool shares_stem = lower(sibling.stem().string()) == stem &&
                             std::ranges::contains(companion_extensions, ext);
    if (!sidecar_of_full_name && !shares_stem) {
      continue;
    }
    // A stem-sharing .mov only belongs to a still image; between two videos
    // (clip.mp4 / clip.mov) it is its own item.
    if (ext == "mov" && !is_still_extension(primary_ext)) {
      continue;
    }
    found.push_back(sibling);
  }
  std::ranges::sort(found);
  return found;
}

} // namespace kustavi
