#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kustavi {
class ImageMeta;
class QualityFlag;
class JunkFlag;
class VideoFlag;
class SimilarGroup;
class Trip;
class TripLeg;
} // namespace kustavi

namespace kustavi::gui {

// Capture and trip times, to the millisecond, on the system clock.
using time_ms = std::chrono::sys_time<std::chrono::milliseconds>;

// WGS84 decimal degrees.
struct geo_point {
  double latitude = 0;
  double longitude = 0;

  [[nodiscard]] friend auto operator==(geo_point const &, geo_point const &) -> bool = default;
};

// Immutable metadata for one scanned image or video. The controller builds it from `ImageMeta`
// events and keeps it as the single source of truth for image metadata.
struct image_info {
  // Path relative to the session folder, such as `2024/paris/IMG_0001.jpg`.
  std::string id;
  // Absolute path of the original file.
  std::string path;
  std::string name;
  std::uint64_t size_bytes = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::optional<time_ms> taken;
  std::optional<geo_point> gps;
  // Absolute path of the working preview that the back end wrote before it sent the event.
  std::string working_image_path;
  bool is_video = false;
  std::optional<std::chrono::milliseconds> video_duration;
};

[[nodiscard]] auto image_info_from(ImageMeta const &meta) -> image_info;

enum class quality_reason : std::uint8_t { blurry, under_exposed, over_exposed };

// The label that the review screens show.
[[nodiscard]] auto label_of(quality_reason reason) -> std::string_view;

struct quality_flag_info {
  std::string image_id;
  std::vector<quality_reason> reasons;
  // Laplacian variance; higher is sharper.
  double sharpness = 0;
  // 0 to 1; 0.5 is ideal.
  double exposure_score = 0;
};

// Throws `std::invalid_argument` for a reason value this build does not know.
[[nodiscard]] auto quality_flag_info_from(QualityFlag const &flag) -> quality_flag_info;

// A photo classified as a non-photograph.
struct junk_flag_info {
  std::string image_id;
  // A short classification, such as "screenshot", "scan" or "meme".
  std::string reason;
  double confidence = 0;
};
[[nodiscard]] auto junk_flag_info_from(JunkFlag const &flag) -> junk_flag_info;

// A video that the video pass flagged.
struct video_flag_info {
  std::string video_id;
  // "too_short", "corrupt", "blurry", "static", or a junk category.
  std::string reason;
  double confidence = 0;
};
[[nodiscard]] auto video_flag_info_from(VideoFlag const &flag) -> video_flag_info;

// A near-duplicate group, members best first.
struct similar_group_info {
  std::uint32_t id = 0;
  std::vector<std::string> member_ids;
  std::string recommended_keep_id;
  // Composite scores, parallel to `member_ids`.
  std::vector<double> member_scores;
};
[[nodiscard]] auto similar_group_info_from(SimilarGroup const &group) -> similar_group_info;

// A contiguous stay at one place within a trip.
struct trip_leg_info {
  // "Rome, Italy"; empty when the back end had no geo table.
  std::string place_name;
  // Filesystem-safe label, unique within the trip.
  std::string slug;
  std::vector<std::string> member_ids;
  std::optional<geo_point> centroid;
};
[[nodiscard]] auto trip_leg_info_from(TripLeg const &leg) -> trip_leg_info;

// A trip cluster, or a month of photos taken at home.
struct trip_info {
  std::uint32_t id = 0;
  time_ms start;
  time_ms end;
  // Member ids in chronological order.
  std::vector<std::string> member_ids;
  // Mean of the members that have GPS.
  std::optional<geo_point> centroid;
  // The folder name for display, such as "Rome, Italy · April 2026".
  std::optional<std::string> folder;
  // The filesystem-safe form of `folder`, used for the committed layout.
  std::string folder_slug;
  // The dominant place, such as "Italy".
  std::string place_name;
  std::vector<trip_leg_info> legs;
  bool is_home = false;
};
[[nodiscard]] auto trip_info_from(Trip const &trip) -> trip_info;

// A named collection of trips, shown as one folder in the trips review.
struct trip_folder_info {
  std::string name;
  std::vector<trip_info> trips;
};

// Photos that are reviewed together: one output folder, or the photos in no trip.
struct batch_info {
  // A stable identifier, which is also the back end's `batch_key`.
  std::string key;
  std::string title;
  std::vector<std::string> image_ids;
};

// Where one pass stands for a batch.
struct batch_pass_status {
  bool done = false;
  // The photos that the pass flagged in the batch.
  int flagged = 0;
};

// One row of the batch menu.
struct batch_summary {
  std::string key;
  std::string title;
  int photo_count = 0;
  int video_count = 0;
  // Status per pass, keyed by the pass's step index.
  std::map<int, batch_pass_status> passes;
};

} // namespace kustavi::gui
