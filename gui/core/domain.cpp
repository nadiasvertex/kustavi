#include "core/domain.h"

#include <stdexcept>
#include <utility>

#include "proto/service.pb.h"

namespace kustavi::gui {

namespace {

auto geo_of(GpsPoint const &point) -> geo_point { return {point.latitude(), point.longitude()}; }

auto time_of(std::int64_t unix_ms) -> time_ms { return time_ms{std::chrono::milliseconds{unix_ms}}; }

auto strings_of(google::protobuf::RepeatedPtrField<std::string> const &values)
    -> std::vector<std::string> {
  return {values.begin(), values.end()};
}

} // namespace

auto image_info_from(ImageMeta const &meta) -> image_info {
  image_info info;
  info.id = meta.id();
  info.path = meta.path();
  info.name = meta.name();
  info.size_bytes = meta.size_bytes();
  info.width = meta.width();
  info.height = meta.height();
  if (meta.has_taken_unix_ms()) {
    info.taken = time_of(meta.taken_unix_ms());
  }
  if (meta.has_gps()) {
    info.gps = geo_of(meta.gps());
  }
  info.working_image_path = meta.thumbnail_path();
  info.is_video = meta.kind() == MediaKind::VIDEO;
  if (meta.has_duration_ms()) {
    info.video_duration = std::chrono::milliseconds{meta.duration_ms()};
  }
  return info;
}

auto label_of(quality_reason reason) -> std::string_view {
  switch (reason) {
  case quality_reason::blurry:
    return "Blurry";
  case quality_reason::under_exposed:
    return "Underexposed";
  case quality_reason::over_exposed:
    return "Overexposed";
  }
  return {};
}

auto quality_flag_info_from(QualityFlag const &flag) -> quality_flag_info {
  quality_flag_info info;
  info.image_id = flag.image_id();
  for (int const value : flag.reasons()) {
    switch (value) {
    case QualityReason::BLURRY:
      info.reasons.push_back(quality_reason::blurry);
      break;
    case QualityReason::UNDER_EXPOSED:
      info.reasons.push_back(quality_reason::under_exposed);
      break;
    case QualityReason::OVER_EXPOSED:
      info.reasons.push_back(quality_reason::over_exposed);
      break;
    default:
      throw std::invalid_argument("unrecognized quality reason: " + std::to_string(value));
    }
  }
  info.sharpness = flag.sharpness();
  info.exposure_score = flag.exposure_score();
  return info;
}

auto junk_flag_info_from(JunkFlag const &flag) -> junk_flag_info {
  return {flag.image_id(), flag.reason(), flag.confidence()};
}

auto video_flag_info_from(VideoFlag const &flag) -> video_flag_info {
  return {flag.video_id(), flag.reason(), flag.confidence()};
}

auto similar_group_info_from(SimilarGroup const &group) -> similar_group_info {
  return {group.id(), strings_of(group.image_ids()), group.recommended_keep_id(),
          {group.member_scores().begin(), group.member_scores().end()}};
}

auto trip_leg_info_from(TripLeg const &leg) -> trip_leg_info {
  trip_leg_info info;
  info.place_name = leg.place_name();
  info.slug = leg.slug();
  info.member_ids = strings_of(leg.image_ids());
  if (leg.has_centroid()) {
    info.centroid = geo_of(leg.centroid());
  }
  return info;
}

auto trip_info_from(Trip const &trip) -> trip_info {
  trip_info info;
  info.id = trip.id();
  info.start = time_of(trip.start_unix_ms());
  info.end = time_of(trip.end_unix_ms());
  info.member_ids = strings_of(trip.image_ids());
  if (trip.has_centroid()) {
    info.centroid = geo_of(trip.centroid());
  }
  if (!trip.folder().empty()) {
    info.folder = trip.folder();
  }
  info.folder_slug = trip.folder_slug();
  info.place_name = trip.place_name();
  for (auto const &leg : trip.legs()) {
    info.legs.push_back(trip_leg_info_from(leg));
  }
  info.is_home = trip.is_home();
  return info;
}

} // namespace kustavi::gui
