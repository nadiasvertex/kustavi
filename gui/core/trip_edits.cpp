#include "core/trip_edits.h"

#include <nlohmann/json.hpp>

namespace kustavi::gui {

namespace {

constexpr int format_version = 1;

} // namespace

auto trip_edits::encode() const -> std::string {
  if (empty()) {
    return {};
  }
  using nlohmann::json;
  auto trips = json::array();
  for (auto const &t : user_trips) {
    trips.push_back({{"id", t.id}, {"startMs", t.start_ms}, {"folder", t.folder},
                     {"place", t.place_name}});
  }
  auto names = json::array();
  for (auto const &r : renames) {
    names.push_back({{"id", r.trip_id}, {"from", r.from}, {"name", r.name}});
  }
  auto membership_json = json::object();
  for (auto const &[image, trip] : membership) {
    membership_json[image] = trip;
  }
  return json{{"v", format_version},
              {"membership", membership_json},
              {"userTrips", trips},
              {"renames", names},
              {"organize", organize_into_folders}}
      .dump();
}

auto trip_edits::decode(std::string_view text) -> std::optional<trip_edits> {
  if (text.empty()) {
    return std::nullopt;
  }
  using nlohmann::json;
  try {
    auto const root = json::parse(text);
    if (!root.is_object() || root.at("v") != format_version) {
      return std::nullopt;
    }
    trip_edits edits;
    for (auto const &[image, trip] : root.at("membership").items()) {
      edits.membership[image] = trip.get<std::int64_t>();
    }
    for (auto const &t : root.at("userTrips")) {
      edits.user_trips.push_back({t.at("id").get<std::int64_t>(), t.at("startMs").get<std::int64_t>(),
                                  t.at("folder").get<std::string>(),
                                  t.at("place").get<std::string>()});
    }
    for (auto const &r : root.at("renames")) {
      edits.renames.push_back({r.at("id").get<std::int64_t>(), r.at("from").get<std::string>(),
                               r.at("name").get<std::string>()});
    }
    edits.organize_into_folders = root.value("organize", true);
    return edits;
  } catch (nlohmann::json::exception const &) {
    return std::nullopt;
  }
}

} // namespace kustavi::gui
