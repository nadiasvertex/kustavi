#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace kustavi::exif {

/** Capture metadata read from an image file. Unavailable fields are
 * std::nullopt (e.g. formats without EXIF, or a missing timestamp). */
struct exif_info {
  std::optional<std::int64_t> taken_unix_ms;
  std::optional<double> latitude;
  std::optional<double> longitude;
};

/** Reads the capture timestamp and GPS position from a JPEG or TIFF file.
 *
 * Timestamps are stored by cameras as local time without a timezone; this
 * reader interprets them in the machine's local timezone. Files of any other
 * format, or files with missing/corrupt metadata, yield empty fields.
 */
auto read_exif(const std::filesystem::path &path) -> exif_info;
} // namespace kustavi::exif
