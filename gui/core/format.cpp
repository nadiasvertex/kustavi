#include "core/format.h"

#include <array>
#include <cmath>
#include <ctime>
#include <format>

namespace kustavi::gui {

auto to_local_civil(time_ms instant) -> civil_time {
  auto const seconds = std::chrono::floor<std::chrono::seconds>(instant).time_since_epoch().count();
  auto const stamp = static_cast<std::time_t>(seconds);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &stamp);
#else
  localtime_r(&stamp, &local);
#endif
  return {local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min};
}

auto format_gb(std::uint64_t bytes) -> std::string {
  return std::format("{:.1f} GB", static_cast<double>(bytes) / 1e9);
}

auto format_bytes(std::uint64_t bytes) -> std::string {
  constexpr double kb = 1024;
  auto const value = static_cast<double>(bytes);
  if (value < kb) {
    return std::format("{} B", bytes);
  }
  if (value < kb * kb) {
    return std::format("{:.1f} KB", value / kb);
  }
  if (value < kb * kb * kb) {
    return std::format("{:.1f} MB", value / kb / kb);
  }
  return std::format("{:.1f} GB", value / kb / kb / kb);
}

auto format_int(std::int64_t value) -> std::string {
  bool const negative = value < 0;
  auto digits = std::to_string(negative ? -static_cast<std::uint64_t>(value)
                                        : static_cast<std::uint64_t>(value));
  std::string out;
  for (std::size_t i = 0; i < digits.size(); ++i) {
    if (i > 0 && (digits.size() - i) % 3 == 0) {
      out += ',';
    }
    out += digits[i];
  }
  return negative ? "-" + out : out;
}

auto format_clock_time(civil_time target, civil_time now) -> std::string {
  bool const same_day =
      target.year == now.year && target.month == now.month && target.day == now.day;
  return same_day ? std::format("{:02}:{:02}", target.hour, target.minute)
                  : format_date_time(target);
}

auto format_date_time(civil_time time) -> std::string {
  return std::format("{}-{:02}-{:02} {:02}:{:02}", time.year, time.month, time.day, time.hour,
                     time.minute);
}

auto junk_timing_caption(std::optional<double> seconds_per_image,
                         std::optional<civil_time> estimated_completion, civil_time now)
    -> std::string {
  if (!seconds_per_image || !estimated_completion) {
    return "Profiling how long the vision model takes on your machine…";
  }
  auto const rate = *seconds_per_image >= 10 ? std::format("{}", std::llround(*seconds_per_image))
                                              : std::format("{:.1f}", *seconds_per_image);
  return std::format("{} seconds per image, estimated completion at {}", rate,
                     format_clock_time(*estimated_completion, now));
}

auto format_duration(time_ms start, time_ms end) -> std::string {
  auto const span = end - start;
  auto const days = std::chrono::duration_cast<std::chrono::days>(span).count();
  auto const total_hours = std::chrono::duration_cast<std::chrono::hours>(span).count();
  auto const hours = total_hours % 24;
  if (days > 0 && hours > 0) {
    return std::format("{}d {}h", days, hours);
  }
  if (days > 0) {
    return std::format("{}d", days);
  }
  return std::format("{}h", total_hours);
}

auto format_media_duration(std::chrono::milliseconds length) -> std::string {
  auto const total = std::chrono::duration_cast<std::chrono::seconds>(length).count();
  auto const hours = total / 3600;
  auto const minutes = total / 60 % 60;
  auto const seconds = total % 60;
  if (hours > 0) {
    return std::format("{}:{:02}:{:02}", hours, minutes, seconds);
  }
  return std::format("{}:{:02}", minutes, seconds);
}

auto format_month_year(time_ms instant) -> std::string {
  constexpr std::array months{"January", "February", "March",     "April",   "May",      "June",
                              "July",    "August",   "September", "October", "November", "December"};
  auto const civil = to_local_civil(instant);
  return std::format("{} {}", months[static_cast<std::size_t>(civil.month - 1)], civil.year);
}

auto slugify(std::string_view text) -> std::string {
  std::string out;
  bool pending_separator = false;
  for (char const raw : text) {
    auto const c = static_cast<unsigned char>(raw);
    bool const digit = c >= '0' && c <= '9';
    bool const upper = c >= 'A' && c <= 'Z';
    bool const lower = c >= 'a' && c <= 'z';
    if (!digit && !upper && !lower) {
      pending_separator = true;
      continue;
    }
    if (pending_separator && !out.empty()) {
      out.push_back('-');
    }
    pending_separator = false;
    out.push_back(upper ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c));
  }
  return out;
}

} // namespace kustavi::gui
