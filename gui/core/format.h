#pragma once

#include "core/domain.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace kustavi::gui {

// A calendar time without a zone, as the user's clock shows it. Formatting takes this instead of
// reading the clock, so the same input gives the same text in tests and on every machine.
struct civil_time {
  int year = 1970;
  int month = 1;
  int day = 1;
  int hour = 0;
  int minute = 0;

  [[nodiscard]] friend auto operator==(civil_time const &, civil_time const &) -> bool = default;
};

// `instant` as the machine's local clock shows it.
[[nodiscard]] auto to_local_civil(time_ms instant) -> civil_time;

// A count of bytes as gigabytes with one decimal, such as `2.9 GB`.
[[nodiscard]] auto format_gb(std::uint64_t bytes) -> std::string;
// A count of bytes in the largest unit that keeps the number at one or more, such as `1.2 MB`.
[[nodiscard]] auto format_bytes(std::uint64_t bytes) -> std::string;
// An integer with commas between thousands, such as `5,000`.
[[nodiscard]] auto format_int(std::int64_t value) -> std::string;
// `HH:mm`, or `yyyy-MM-dd HH:mm` when `target` is on another day than `now`.
[[nodiscard]] auto format_clock_time(civil_time target, civil_time now) -> std::string;
// `yyyy-MM-dd HH:mm`.
[[nodiscard]] auto format_date_time(civil_time time) -> std::string;
// What the junk pass expects to take. Without a measured rate it says that it is still measuring.
[[nodiscard]] auto junk_timing_caption(std::optional<double> seconds_per_image,
                                       std::optional<civil_time> estimated_completion,
                                       civil_time now) -> std::string;
// The span between two times, such as `3d 14h`.
[[nodiscard]] auto format_duration(time_ms start, time_ms end) -> std::string;
// A clip length as `m:ss`, or `h:mm:ss` from one hour.
[[nodiscard]] auto format_media_duration(std::chrono::milliseconds length) -> std::string;

} // namespace kustavi::gui
