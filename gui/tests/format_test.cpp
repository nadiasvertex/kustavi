#include <catch2/catch_test_macros.hpp>

#include "core/format.h"

using namespace kustavi::gui;
using namespace std::chrono_literals;

namespace {

constexpr civil_time today{2026, 10, 23, 12, 0};

} // namespace

TEST_CASE("a time on the same day shows only the hour and minute", "[format]") {
  CHECK(format_clock_time({2026, 10, 23, 9, 5}, today) == "09:05");
}

TEST_CASE("a time on another day shows the date", "[format]") {
  CHECK(format_clock_time({2026, 10, 25, 14, 30}, today) == "2026-10-25 14:30");
  CHECK(format_clock_time({2025, 10, 23, 14, 30}, today) == "2025-10-23 14:30");
}

TEST_CASE("the junk timing caption says it is measuring until it has a rate", "[format]") {
  CHECK(junk_timing_caption(std::nullopt, std::nullopt, today) ==
        "Profiling how long the vision model takes on your machine…");
  CHECK(junk_timing_caption(2.4, std::nullopt, today) ==
        "Profiling how long the vision model takes on your machine…");
}

TEST_CASE("a rate below ten seconds keeps one decimal", "[format]") {
  CHECK(junk_timing_caption(2.4, civil_time{2026, 10, 23, 10, 15}, today) ==
        "2.4 seconds per image, estimated completion at 10:15");
}

TEST_CASE("a rate of ten seconds or more is a whole number", "[format]") {
  CHECK(junk_timing_caption(12.7, civil_time{2026, 10, 23, 23, 0}, today) ==
        "13 seconds per image, estimated completion at 23:00");
  CHECK(junk_timing_caption(10.0, civil_time{2026, 10, 24, 1, 0}, today) ==
        "10 seconds per image, estimated completion at 2026-10-24 01:00");
}

TEST_CASE("byte counts use the unit that fits", "[format]") {
  CHECK(format_bytes(0) == "0 B");
  CHECK(format_bytes(1023) == "1023 B");
  CHECK(format_bytes(1024) == "1.0 KB");
  CHECK(format_bytes(1536) == "1.5 KB");
  CHECK(format_bytes(5 * 1024 * 1024) == "5.0 MB");
  CHECK(format_bytes(3ULL * 1024 * 1024 * 1024) == "3.0 GB");
  CHECK(format_gb(2'900'000'000) == "2.9 GB");
}

TEST_CASE("integers get commas between thousands", "[format]") {
  CHECK(format_int(0) == "0");
  CHECK(format_int(999) == "999");
  CHECK(format_int(1000) == "1,000");
  CHECK(format_int(5000) == "5,000");
  CHECK(format_int(1234567) == "1,234,567");
  CHECK(format_int(-1234567) == "-1,234,567");
  CHECK(format_int(-12) == "-12");
}

TEST_CASE("a date and time has fixed width fields", "[format]") {
  CHECK(format_date_time({2026, 1, 2, 3, 4}) == "2026-01-02 03:04");
}

TEST_CASE("a duration shows days and hours", "[format]") {
  auto const start = time_ms{};
  CHECK(format_duration(start, start + 86h + 14h) == "4d 4h");
  CHECK(format_duration(start, start + 72h) == "3d");
  CHECK(format_duration(start, start + 5h + 59min) == "5h");
  CHECK(format_duration(start, start) == "0h");
}

TEST_CASE("a media length is minutes and seconds, with hours from one hour", "[format]") {
  CHECK(format_media_duration(0ms) == "0:00");
  CHECK(format_media_duration(65s) == "1:05");
  CHECK(format_media_duration(59'999ms) == "0:59");
  CHECK(format_media_duration(3600s) == "1:00:00");
  CHECK(format_media_duration(3725s) == "1:02:05");
}

TEST_CASE("a time converts to the local calendar", "[format]") {
  // Noon UTC is the same day in every zone within a few hours of UTC; check shape, not zone.
  auto const civil = to_local_civil(time_ms{std::chrono::milliseconds{1'700'000'000'000}});
  CHECK(civil.year == 2023);
  CHECK(civil.month == 11);
  CHECK(civil.day >= 14);
  CHECK(civil.day <= 15);
}
