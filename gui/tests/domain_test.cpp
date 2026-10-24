#include <catch2/catch_test_macros.hpp>

#include "core/backend_error.h"
#include "core/domain.h"
#include "proto/service.pb.h"

using namespace kustavi::gui;
using namespace std::chrono_literals;

namespace {

auto meta(std::string id) -> kustavi::ImageMeta {
  kustavi::ImageMeta m;
  m.set_id(id);
  m.set_path("/photos/" + id);
  m.set_name(id);
  m.set_width(4000);
  m.set_height(3000);
  m.set_size_bytes(123456);
  return m;
}

} // namespace

TEST_CASE("image metadata maps the working preview path and the optional fields", "[domain]") {
  auto m = meta("a.jpg");
  m.set_thumbnail_path("/cache/a.jpg");
  m.set_taken_unix_ms(1'700'000'000'000);
  m.mutable_gps()->set_latitude(48.85);
  m.mutable_gps()->set_longitude(2.35);
  auto const info = image_info_from(m);
  CHECK(info.id == "a.jpg");
  CHECK(info.path == "/photos/a.jpg");
  CHECK(info.size_bytes == 123456);
  CHECK(info.width == 4000);
  CHECK(info.working_image_path == "/cache/a.jpg");
  REQUIRE(info.taken.has_value());
  CHECK(info.taken->time_since_epoch() == 1'700'000'000'000ms);
  CHECK(info.gps == geo_point{48.85, 2.35});
  CHECK_FALSE(info.is_video);
  CHECK_FALSE(info.video_duration.has_value());
}

TEST_CASE("absent optional metadata maps to nothing", "[domain]") {
  auto const info = image_info_from(meta("a.jpg"));
  CHECK_FALSE(info.taken.has_value());
  CHECK_FALSE(info.gps.has_value());
}

TEST_CASE("a video has a kind and a length", "[domain]") {
  auto m = meta("clip.mp4");
  m.set_kind(kustavi::MediaKind::VIDEO);
  m.set_duration_ms(61'500);
  auto const info = image_info_from(m);
  CHECK(info.is_video);
  CHECK(info.video_duration == 61'500ms);
}

TEST_CASE("a quality flag maps its reasons and scores", "[domain]") {
  kustavi::QualityFlag flag;
  flag.set_image_id("a.jpg");
  flag.add_reasons(kustavi::QualityReason::BLURRY);
  flag.add_reasons(kustavi::QualityReason::OVER_EXPOSED);
  flag.set_sharpness(7.5);
  flag.set_exposure_score(0.88);
  auto const info = quality_flag_info_from(flag);
  CHECK(info.image_id == "a.jpg");
  CHECK(info.reasons ==
        std::vector{quality_reason::blurry, quality_reason::over_exposed});
  CHECK(info.sharpness == 7.5);
  CHECK(info.exposure_score == 0.88);
}

TEST_CASE("an unknown quality reason is refused", "[domain]") {
  kustavi::QualityFlag flag;
  flag.add_reasons(kustavi::QualityReason::QUALITY_REASON_UNSPECIFIED);
  CHECK_THROWS_AS(quality_flag_info_from(flag), std::invalid_argument);
}

TEST_CASE("quality reasons have review labels", "[domain]") {
  CHECK(label_of(quality_reason::blurry) == "Blurry");
  CHECK(label_of(quality_reason::under_exposed) == "Underexposed");
  CHECK(label_of(quality_reason::over_exposed) == "Overexposed");
}

TEST_CASE("junk and video flags map their fields", "[domain]") {
  kustavi::JunkFlag junk;
  junk.set_image_id("s.png");
  junk.set_reason("screenshot");
  junk.set_confidence(0.75);
  auto const j = junk_flag_info_from(junk);
  CHECK(j.image_id == "s.png");
  CHECK(j.reason == "screenshot");
  CHECK(j.confidence == 0.75);

  kustavi::VideoFlag video;
  video.set_video_id("v.mp4");
  video.set_reason("too_short");
  video.set_confidence(1.0);
  auto const v = video_flag_info_from(video);
  CHECK(v.video_id == "v.mp4");
  CHECK(v.reason == "too_short");
}

TEST_CASE("a similar group keeps its members, scores and keeper", "[domain]") {
  kustavi::SimilarGroup group;
  group.set_id(3);
  group.add_image_ids("b.jpg");
  group.add_image_ids("a.jpg");
  group.set_recommended_keep_id("b.jpg");
  group.add_member_scores(0.9);
  group.add_member_scores(0.8);
  auto const info = similar_group_info_from(group);
  CHECK(info.id == 3);
  CHECK(info.member_ids == std::vector<std::string>{"b.jpg", "a.jpg"});
  CHECK(info.recommended_keep_id == "b.jpg");
  CHECK(info.member_scores == std::vector<double>{0.9, 0.8});
}

TEST_CASE("a trip maps its times, folder, legs and home flag", "[domain]") {
  kustavi::Trip trip;
  trip.set_id(7);
  trip.set_start_unix_ms(1'000);
  trip.set_end_unix_ms(2'000);
  trip.add_image_ids("a.jpg");
  trip.set_folder("Rome, Italy · April 2026");
  trip.set_folder_slug("rome-italy-april-2026");
  trip.set_place_name("Italy");
  trip.set_is_home(false);
  auto *leg = trip.add_legs();
  leg->set_place_name("Rome, Italy");
  leg->set_slug("rome");
  leg->add_image_ids("a.jpg");
  leg->mutable_centroid()->set_latitude(41.9);
  leg->mutable_centroid()->set_longitude(12.5);
  auto const info = trip_info_from(trip);
  CHECK(info.id == 7);
  CHECK(info.start.time_since_epoch() == 1000ms);
  CHECK(info.end.time_since_epoch() == 2000ms);
  CHECK(info.folder == "Rome, Italy · April 2026");
  CHECK(info.folder_slug == "rome-italy-april-2026");
  CHECK_FALSE(info.centroid.has_value());
  REQUIRE(info.legs.size() == 1);
  CHECK(info.legs[0].slug == "rome");
  CHECK(info.legs[0].centroid == geo_point{41.9, 12.5});
}

TEST_CASE("an empty folder name maps to no folder", "[domain]") {
  kustavi::Trip trip;
  trip.set_is_home(true);
  auto const info = trip_info_from(trip);
  CHECK_FALSE(info.folder.has_value());
  CHECK(info.is_home);
}

TEST_CASE("an engine status becomes a back-end error", "[domain][error]") {
  auto const missing = to_backend_error({status_code::not_found, "missing"});
  CHECK(missing.not_found());
  CHECK(message_of(missing) == "missing");

  auto const boom = to_backend_error({status_code::internal, "boom"});
  CHECK_FALSE(boom.not_found());
  CHECK(boom.code == status_code::internal);
  CHECK(message_of(boom) == "boom");
}
