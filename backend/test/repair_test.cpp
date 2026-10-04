// Assertions for the metadata repair heuristics: filename dates, GPS
// borrowing and camera clock offset detection. Exits non-zero on failure.
// Wired into `just test-backend` via //backend:repair_test.

#include "pass/repair.h"

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

using namespace kustavi;

int g_failures = 0;

void check(bool ok, const char *what) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) {
    ++g_failures;
  }
}

constexpr std::int64_t k_minute_ms = 60'000;
constexpr std::int64_t k_hour_ms = 60 * k_minute_ms;

auto local_ms(int y, int mo, int d, int h, int mi, int s) -> std::int64_t {
  std::tm tm{};
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = s;
  tm.tm_isdst = -1;
  return static_cast<std::int64_t>(std::mktime(&tm)) * 1000;
}

void test_filename_dates() {
  const auto expect = local_ms(2019, 7, 4, 12, 34, 56);
  for (const char *name :
       {"IMG_20190704_123456.jpg", "PXL_20190704_123456789.jpg",
        "WhatsApp Image 2019-07-04 at 12.34.56.jpeg",
        "Screenshot_2019-07-04-12-34-56.png", "20190704123456.jpg",
        "VID_20190704_123456.mp4"}) {
    const auto parsed = parse_filename_date(name);
    check(parsed && parsed->unix_ms == expect && parsed->has_time, name);
  }

  const auto date_only = parse_filename_date("IMG-20190704-WA0001.jpg");
  check(date_only && !date_only->has_time &&
            date_only->unix_ms == local_ms(2019, 7, 4, 12, 0, 0),
        "date-only name gets noon and has_time = false");

  check(!parse_filename_date("IMG_0001.jpg"), "plain counter has no date");
  check(!parse_filename_date("1562243696123.jpg"), "epoch-ms name has no date");
  check(!parse_filename_date("IMG_20191340_000000.jpg"),
        "impossible month/day is rejected");
}

auto input(std::string id, std::string camera, std::optional<std::int64_t> exif,
           bool gps = false) -> repair_input {
  repair_input in{.id = std::move(id),
                  .file_name = "x.jpg",
                  .camera = std::move(camera),
                  .exif_taken_ms = exif};
  if (gps) {
    in.exif_latitude = 48.85;
    in.exif_longitude = 2.35;
  }
  return in;
}

void test_date_fallbacks() {
  std::vector<repair_input> in;
  in.push_back(input("a", "Cam", 1'000'000));
  repair_input named{
      .id = "b", .file_name = "IMG_20190704_123456.jpg", .modified_ms = 5};
  in.push_back(named);
  repair_input modified{.id = "c", .file_name = "scan.jpg", .modified_ms = 777};
  in.push_back(modified);
  in.push_back({.id = "d", .file_name = "none.jpg"});

  const auto out = repair_metadata(in, {});
  check(out[0].date_source == "exif" && out[0].taken_unix_ms == 1'000'000,
        "exif date wins");
  check(out[1].date_source == "filename" &&
            out[1].taken_unix_ms == local_ms(2019, 7, 4, 12, 34, 56),
        "filename date beats modified time");
  check(out[2].date_source == "modified" && out[2].taken_unix_ms == 777,
        "modified time is the last resort");
  check(out[3].date_source.empty() && !out[3].taken_unix_ms,
        "no date stays empty");
}

void test_offset_application() {
  repair_params params;
  params.offset_minutes_by_camera["Dslr"] = -300;
  const auto out = repair_metadata(
      {input("a", "Dslr", 10 * k_hour_ms), input("b", "Phone", 10 * k_hour_ms)},
      params);
  check(out[0].taken_unix_ms == 5 * k_hour_ms &&
            out[0].date_source == "exif+offset",
        "offset moves the chosen camera");
  check(out[1].taken_unix_ms == 10 * k_hour_ms && out[1].date_source == "exif",
        "other cameras are untouched");
}

void test_gps_borrowing() {
  const std::int64_t t0 = 100 * k_hour_ms;
  std::vector<repair_input> in = {
      input("phone", "Phone", t0, true),
      input("near", "Dslr", t0 + 4 * k_minute_ms),
      input("far", "Dslr", t0 + 30 * k_minute_ms),
  };
  auto out = repair_metadata(in, {});
  check(out[0].gps_source == "exif", "own GPS is kept");
  check(out[1].gps_source == "inferred" && out[1].latitude == 48.85,
        "photo within the window borrows GPS");
  check(out[2].gps_source.empty() && !out[2].latitude,
        "photo outside the window gets none");

  // Donors on both sides that disagree: ambiguous, so nothing is borrowed.
  repair_input before = input("before", "Phone", t0, true);
  repair_input after = input("after", "Phone", t0 + 8 * k_minute_ms, true);
  after.exif_latitude = 35.0;
  after.exif_longitude = 139.0;
  out = repair_metadata(
      {before, after, input("mid", "Dslr", t0 + 4 * k_minute_ms)}, {});
  check(out[2].gps_source.empty(),
        "far-apart donors on both sides are ambiguous");

  // A date from modified time is not trusted enough to borrow by.
  repair_input stale{
      .id = "stale", .file_name = "scan.jpg", .modified_ms = t0 + k_minute_ms};
  out = repair_metadata({input("phone", "Phone", t0, true), stale}, {});
  check(out[1].gps_source.empty(), "modified-time dates never borrow GPS");

  // An accepted offset makes the DSLR line up with the phone.
  repair_params params;
  params.offset_minutes_by_camera["Dslr"] = 60;
  out = repair_metadata({input("phone", "Phone", t0, true),
                         input("dslr", "Dslr", t0 - 56 * k_minute_ms)},
                        params);
  check(out[1].gps_source == "inferred", "shifted time can borrow GPS");
}

void test_clock_offset_detection() {
  // A phone that photographs in bursts during outings, and a DSLR whose clock
  // is 3 hours slow and shoots at the same outings.
  std::vector<repair_input> in;
  const std::int64_t day = 24 * k_hour_ms;
  for (int outing = 0; outing < 12; ++outing) {
    const std::int64_t start = (200 + outing * 3) * day + 14 * k_hour_ms;
    for (int shot = 0; shot < 6; ++shot) {
      in.push_back(input("p", "Phone", start + shot * 3 * k_minute_ms, true));
      in.push_back(
          input("d", "Dslr", start + shot * 3 * k_minute_ms - 3 * k_hour_ms));
    }
  }
  auto found = detect_clock_offsets(in);
  check(found.size() == 1 && found[0].camera == "Dslr" &&
            found[0].offset_minutes == 180,
        "slow DSLR clock is detected (+180 minutes)");
  check(!found.empty() && found[0].matched >= 60 &&
            found[0].matched_unshifted == 0,
        "evidence counts reflect the shift");

  // The same camera with a correct clock yields nothing.
  std::vector<repair_input> aligned;
  for (const auto &item : in) {
    auto copy = item;
    if (copy.camera == "Dslr") {
      *copy.exif_taken_ms += 3 * k_hour_ms;
    }
    aligned.push_back(copy);
  }
  check(detect_clock_offsets(aligned).empty(),
        "aligned clocks give no suggestion");

  // A phone that shoots around the clock matches at every shift: no evidence.
  std::vector<repair_input> dense;
  for (int i = 0; i < 4000; ++i) {
    dense.push_back(input("p", "Phone", 300 * day + i * 5 * k_minute_ms, true));
  }
  for (int i = 0; i < 20; ++i) {
    dense.push_back(input("d", "Dslr", 300 * day + i * 977 * k_minute_ms));
  }
  check(detect_clock_offsets(dense).empty(),
        "an always-shooting reference gives no suggestion");
}

} // namespace

int main() {
  test_filename_dates();
  test_date_fallbacks();
  test_offset_application();
  test_gps_borrowing();
  test_clock_offset_detection();

  if (g_failures > 0) {
    std::printf("\n%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("\nall checks passed\n");
  return 0;
}
