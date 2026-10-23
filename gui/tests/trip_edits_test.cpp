#include <catch2/catch_test_macros.hpp>

#include "core/trip_edits.h"

using namespace kustavi::gui;

TEST_CASE("edits that change nothing encode to nothing", "[trip_edits]") {
  CHECK(trip_edits{}.empty());
  CHECK(trip_edits{}.encode().empty());
}

TEST_CASE("turning folders off is an edit", "[trip_edits]") {
  trip_edits edits;
  edits.organize_into_folders = false;
  CHECK_FALSE(edits.empty());
  auto const decoded = trip_edits::decode(edits.encode());
  REQUIRE(decoded.has_value());
  CHECK_FALSE(decoded->organize_into_folders);
}

TEST_CASE("edits survive a round trip", "[trip_edits]") {
  trip_edits edits;
  edits.membership = {{"a.jpg", 3}, {"b.jpg", -1}};
  edits.user_trips = {{900, 1'700'000'000'000, "Camping · June 2026", "Yosemite"}};
  edits.renames = {{3, "Rome · April 2026", "Roma 2026"}};
  edits.organize_into_folders = false;
  auto const decoded = trip_edits::decode(edits.encode());
  REQUIRE(decoded.has_value());
  CHECK(*decoded == edits);
}

TEST_CASE("text that is empty or not an encoding decodes to nothing", "[trip_edits]") {
  CHECK_FALSE(trip_edits::decode("").has_value());
  CHECK_FALSE(trip_edits::decode("not json").has_value());
  CHECK_FALSE(trip_edits::decode("[]").has_value());
  CHECK_FALSE(trip_edits::decode(R"({"v":2,"membership":{},"userTrips":[],"renames":[]})")
                  .has_value());
  CHECK_FALSE(trip_edits::decode(R"({"v":1,"membership":{"a":"x"},"userTrips":[],"renames":[]})")
                  .has_value());
  CHECK_FALSE(trip_edits::decode(R"({"v":1})").has_value());
}

TEST_CASE("folders are on unless the text says otherwise", "[trip_edits]") {
  auto const decoded = trip_edits::decode(
      R"({"v":1,"membership":{"a.jpg":2},"userTrips":[],"renames":[]})");
  REQUIRE(decoded.has_value());
  CHECK(decoded->organize_into_folders);
  CHECK(decoded->membership.at("a.jpg") == 2);
}

TEST_CASE("text that the Flutter GUI wrote decodes", "[trip_edits]") {
  // The shape that `TripEdits.encode` in the Dart GUI writes.
  auto const decoded = trip_edits::decode(
      R"({"v":1,"membership":{"x.jpg":-1},"userTrips":[{"id":5,"startMs":10,"folder":"F","place":"P"}],)"
      R"("renames":[{"id":1,"from":"A","name":"B"}],"organize":true})");
  REQUIRE(decoded.has_value());
  CHECK(decoded->user_trips == std::vector<user_trip_edit>{{5, 10, "F", "P"}});
  CHECK(decoded->renames == std::vector<folder_rename_edit>{{1, "A", "B"}});
}
