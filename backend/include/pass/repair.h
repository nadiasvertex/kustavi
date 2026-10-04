#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kustavi {

/** A date read from a file name, interpreted in the machine's local time. */
struct filename_date {
  std::int64_t unix_ms = 0;
  bool has_time = false; //! False for date-only names; the time is then noon.
};

/** Finds a capture date in names such as `IMG_20190704_123456.jpg`,
 * `PXL_20190704_123456789.jpg`, `WhatsApp Image 2019-07-04 at 12.34.56.jpeg`
 * or `IMG-20190704-WA0001.jpg`. Years 1990-2099 are accepted. */
auto parse_filename_date(std::string_view file_name)
    -> std::optional<filename_date>;

/** What the repair pass knows about one image before repairing it. */
struct repair_input {
  std::string id;
  std::string file_name;
  std::string camera;                         //! EXIF make/model; may be empty.
  std::optional<std::int64_t> exif_taken_ms;  //! Time stored in the file.
  std::optional<std::int64_t> modified_ms;    //! File modification time.
  std::optional<double> exif_latitude;
  std::optional<double> exif_longitude;
};

/** Repair tunables. */
struct repair_params {
  /** Minutes to add to the EXIF times of each camera. */
  std::unordered_map<std::string, int> offset_minutes_by_camera;
  /** Largest time distance at which GPS is borrowed from another photo. */
  int gps_window_minutes = 10;
};

/** The repaired date and position of one image. `date_source` is "exif",
 * "exif+offset", "filename", "modified" or empty (no date); `gps_source` is
 * "exif", "inferred" or empty (no position). */
struct repaired_metadata {
  std::string id;
  std::optional<std::int64_t> taken_unix_ms;
  std::optional<double> latitude;
  std::optional<double> longitude;
  std::string date_source;
  std::string gps_source;
};

/** A proposed clock correction for one camera. */
struct clock_offset_suggestion {
  std::string camera;
  int offset_minutes = 0;          //! Add to the camera's times.
  std::size_t photos = 0;          //! Photos from the camera with an EXIF time.
  std::size_t matched = 0;         //! Photos near a reference photo after the shift.
  std::size_t matched_unshifted = 0; //! The same count with no shift.
};

/** Repairs every image's date and position.
 *
 * Dates: the EXIF time wins; otherwise a time in the file name; otherwise the
 * file modification time. A camera's accepted offset moves its EXIF times
 * only.
 *
 * Positions: an image without GPS borrows the position of the nearest photo
 * that has EXIF GPS, provided both have a trustworthy time (EXIF, shifted
 * EXIF, or a file name with a time of day) within `gps_window_minutes`. When
 * photos on both sides qualify and lie more than 2 km apart, the position is
 * ambiguous and left empty. Borrowed positions are never borrowed again.
 *
 * Results are in input order.
 */
auto repair_metadata(const std::vector<repair_input> &inputs,
                     const repair_params &params)
    -> std::vector<repaired_metadata>;

/** Looks for cameras whose clock appears to be off.
 *
 * GPS-tagged cameras (most of whose photos carry GPS, typically phones) act
 * as the reference clock. For every other camera with at least
 * `min_photos` EXIF-timed photos, each whole-minute shift within +-14 hours
 * is scored by how many of the camera's photos land within five minutes of a
 * reference photo. A shift is suggested when it clearly beats both no shift
 * and the typical shift, so a reference that photographs all day (and so
 * matches at any shift) never produces a suggestion.
 */
auto detect_clock_offsets(const std::vector<repair_input> &inputs,
                          std::size_t min_photos = 10)
    -> std::vector<clock_offset_suggestion>;

} // namespace kustavi
