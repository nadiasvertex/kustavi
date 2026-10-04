#pragma once

#include <array>
#include <filesystem>
#include <string_view>
#include <vector>

namespace kustavi {

/** Extensions of files that travel with a photo or video: RAW originals, HEIC
 * stills, Live Photo motion clips and edit sidecars (.xmp, .aae, ...). */
inline constexpr std::array<std::string_view, 17> companion_extensions{
    "cr2", "cr3", "nef", "arw", "dng", "orf", "rw2", "raf", "srw",
    "pef", "heic", "heif", "mov", "xmp", "aae", "dop", "pp3"};

/** Siblings of `primary` that belong with it, sorted by path.
 *
 * A sibling qualifies when it sits in the same directory, has a companion
 * extension, and either shares the primary's stem (`IMG_1.jpg` and
 * `IMG_1.CR2`, `IMG_1.mov`, `IMG_1.xmp`) or is a sidecar named after the full
 * file name (`IMG_1.jpg.xmp`). Stems and extensions compare
 * case-insensitively. Files that are items in their own right (another still
 * image, or a video that is not a Live Photo clip) are never returned, so an
 * unrelated `IMG_1.png` stays a separate item.
 */
auto find_companions(const std::filesystem::path &primary)
    -> std::vector<std::filesystem::path>;

/** True for a video that is the motion half of a Live Photo: a `.mov` whose
 * directory also holds a still image with the same stem that the scan
 * ingests (jpg, jpeg, png, webp). Such a clip is not offered as a separate
 * item; it is a companion of the still. */
auto is_live_photo_motion(const std::filesystem::path &path) -> bool;

} // namespace kustavi
