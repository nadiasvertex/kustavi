#include <catch2/catch_test_macros.hpp>

#include "core/format.h"
#include "core/trips_plan.h"

#include <algorithm>

using namespace kustavi::gui;
using namespace std::chrono_literals;

namespace {

auto ms(std::int64_t value) -> time_ms { return time_ms{std::chrono::milliseconds{value}}; }

auto trip(std::uint32_t id, std::vector<std::string> members, std::string folder,
          std::string slug = {}, std::string place = {}, std::int64_t start = 0,
          std::int64_t end = 0) -> trip_info {
  trip_info t;
  t.id = id;
  t.start = ms(start);
  t.end = ms(end);
  t.member_ids = std::move(members);
  t.folder = std::move(folder);
  t.folder_slug = std::move(slug);
  t.place_name = std::move(place);
  return t;
}

auto ids_of(std::vector<trip_info> const &trips) -> std::vector<std::uint32_t> {
  std::vector<std::uint32_t> ids;
  std::ranges::transform(trips, std::back_inserter(ids), [](auto const &t) { return t.id; });
  return ids;
}

// Three photos: a and b in a Rome trip, c in an Oslo trip.
struct scene {
  scene() {
    for (auto const *id : {"a.jpg", "b.jpg", "c.jpg"}) {
      image_info info;
      info.id = id;
      info.name = id;
      images[id] = info;
      ordered.push_back(id);
    }
    plan.set_clustering({trip(0, {"a.jpg", "b.jpg"}, "Rome, Italy · April 2026", "", "Rome, Italy", 1000, 2000),
                         trip(1, {"c.jpg"}, "Oslo, Norway · May 2026", "", "Oslo, Norway", 9000, 9000)});
  }

  [[nodiscard]] auto context() const -> trip_context { return {images, ordered, deleted}; }
  [[nodiscard]] auto members(std::uint32_t id) const -> std::vector<std::string> {
    for (auto const &t : plan.effective_trips(context())) {
      if (t.id == id) {
        return t.member_ids;
      }
    }
    return {};
  }

  std::unordered_map<std::string, image_info> images;
  std::vector<std::string> ordered;
  std::unordered_set<std::string> deleted;
  trips_plan plan;
};

} // namespace

TEST_CASE("an untouched clustering is the effective trips", "[trips_plan]") {
  scene s;
  auto const trips = s.plan.effective_trips(s.context());
  CHECK(ids_of(trips) == std::vector<std::uint32_t>{0, 1});
  CHECK(trips[0].member_ids == std::vector<std::string>{"a.jpg", "b.jpg"});
  CHECK(trips[0].start == ms(1000));
}

TEST_CASE("moving a photo to another trip changes both trips", "[trips_plan]") {
  scene s;
  s.plan.move_images({"b.jpg"}, 1);
  CHECK(s.members(0) == std::vector<std::string>{"a.jpg"});
  auto const oslo = s.members(1);
  CHECK(oslo.size() == 2);
  CHECK(std::ranges::find(oslo, "b.jpg") != oslo.end());
}

TEST_CASE("pulling the last photo out removes the trip", "[trips_plan]") {
  scene s;
  s.plan.move_images({"c.jpg"}, std::nullopt);
  CHECK(ids_of(s.plan.effective_trips(s.context())) == std::vector<std::uint32_t>{0});
  CHECK(s.plan.unassigned_image_ids(s.context()) == std::vector<std::string>{"c.jpg"});
}

TEST_CASE("a photo marked for deletion leaves its trip and is not unassigned", "[trips_plan]") {
  scene s;
  s.deleted = {"b.jpg"};
  CHECK(s.members(0) == std::vector<std::string>{"a.jpg"});
  CHECK(s.plan.unassigned_image_ids(s.context()).empty());
  s.deleted.insert("c.jpg");
  CHECK(ids_of(s.plan.effective_trips(s.context())) == std::vector<std::uint32_t>{0});
  CHECK(s.plan.marked_count(s.context()) == 2);
}

TEST_CASE("photos in no trip are unassigned, in scan order", "[trips_plan]") {
  scene s;
  image_info extra;
  extra.id = "z.jpg";
  s.images["z.jpg"] = extra;
  s.ordered.push_back("z.jpg");
  s.plan.move_images({"a.jpg"}, std::nullopt);
  CHECK(s.plan.unassigned_image_ids(s.context()) ==
        std::vector<std::string>{"a.jpg", "z.jpg"});
}

TEST_CASE("a trip made by hand takes its photos and the place they came from", "[trips_plan]") {
  scene s;
  s.plan.move_images({"a.jpg"}, std::nullopt);
  auto const id = s.plan.create_trip({"a.jpg"}, s.context(), ms(0));
  CHECK(id == trips_plan::first_user_trip_id);
  auto const trips = s.plan.effective_trips(s.context());
  auto const made = std::ranges::find(trips, id, [](auto const &t) { return t.id; });
  REQUIRE(made != trips.end());
  // The photo was clustered under Rome, so the new trip borrows that place.
  CHECK(made->folder->starts_with("Rome, Italy"));
  CHECK(s.plan.unassigned_image_ids(s.context()).empty());
  CHECK(s.plan.create_trip({"b.jpg"}, s.context(), ms(0)) == id + 1);
}

TEST_CASE("a hand-made trip without a place is named for the month or is a new trip",
          "[trips_plan]") {
  scene s;
  s.images["a.jpg"].taken = ms(1'700'000'000'000);
  image_info loose;
  loose.id = "loose.jpg";
  s.images["loose.jpg"] = loose;
  s.ordered.push_back("loose.jpg");
  s.plan.set_clustering({});
  auto const dated = s.plan.create_trip({"a.jpg"}, s.context(), ms(0));
  auto const undated = s.plan.create_trip({"loose.jpg"}, s.context(), ms(0));
  auto const trips = s.plan.effective_trips(s.context());
  auto const folder_of = [&](std::int64_t id) {
    return std::ranges::find(trips, id, [](auto const &t) { return t.id; })->folder.value();
  };
  CHECK(folder_of(dated).starts_with("Trip · "));
  CHECK(folder_of(dated).ends_with("2023"));
  CHECK(folder_of(undated) == "New trip");
}

TEST_CASE("a trip that lost a photo takes its times and centroid from those that are left",
          "[trips_plan]") {
  scene s;
  s.images["a.jpg"].taken = ms(1500);
  s.images["a.jpg"].gps = geo_point{10, 20};
  s.images["b.jpg"].taken = ms(1800);
  s.images["b.jpg"].gps = geo_point{30, 40};
  s.plan.move_images({"b.jpg"}, std::nullopt);
  auto const trips = s.plan.effective_trips(s.context());
  CHECK(trips[0].start == ms(1500));
  CHECK(trips[0].end == ms(1500));
  CHECK(trips[0].centroid == geo_point{10, 20});
  REQUIRE(trips[0].legs.size() == 1);
  CHECK(trips[0].legs[0].member_ids == std::vector<std::string>{"a.jpg"});
}

TEST_CASE("a trip with several legs keeps them when photos only leave", "[trips_plan]") {
  scene s;
  auto multi = trip(5, {"a.jpg", "b.jpg", "c.jpg"}, "Trip", "trip", "Italy", 100, 300);
  multi.legs = {{"Rome, Italy", "rome", {"a.jpg", "b.jpg"}, std::nullopt},
                {"Florence, Italy", "florence", {"c.jpg"}, std::nullopt}};
  s.plan.set_clustering({multi});
  s.plan.move_images({"c.jpg"}, std::nullopt);
  auto const trips = s.plan.effective_trips(s.context());
  REQUIRE(trips.size() == 1);
  // One leg left with no photos and is dropped; the other keeps its name.
  REQUIRE(trips[0].legs.size() == 1);
  CHECK(trips[0].legs[0].slug == "rome");
}

TEST_CASE("a trip that gained a photo becomes one leg", "[trips_plan]") {
  scene s;
  auto multi = trip(5, {"a.jpg", "b.jpg"}, "Trip", "trip", "Italy", 100, 300);
  multi.legs = {{"Rome", "rome", {"a.jpg"}, std::nullopt}, {"Florence", "florence", {"b.jpg"}, std::nullopt}};
  s.plan.set_clustering({multi, trip(6, {"c.jpg"}, "Other")});
  s.plan.move_images({"c.jpg"}, 5);
  auto const trips = s.plan.effective_trips(s.context());
  auto const found = std::ranges::find(trips, 5U, &trip_info::id);
  REQUIRE(found != trips.end());
  REQUIRE(found->legs.size() == 1);
  CHECK(found->legs[0].slug == "trip");
  CHECK(found->legs[0].member_ids.size() == 3);
}

TEST_CASE("trips sort by start time and folders by name", "[trips_plan]") {
  scene s;
  s.plan.set_clustering({trip(0, {"a.jpg"}, "Zurich", "", "", 5000, 5000),
                         trip(1, {"b.jpg"}, "Athens", "", "", 9000, 9000),
                         trip(2, {"c.jpg"}, "Zurich", "", "", 1000, 1000)});
  CHECK(ids_of(s.plan.effective_trips(s.context())) == std::vector<std::uint32_t>{2, 0, 1});
  auto const folders = s.plan.folders(s.context());
  REQUIRE(folders.size() == 2);
  CHECK(folders[0].name == "Athens");
  CHECK(folders[1].name == "Zurich");
  CHECK(ids_of(folders[1].trips) == std::vector<std::uint32_t>{2, 0});
}

TEST_CASE("renaming a folder moves its trips to the new name", "[trips_plan]") {
  scene s;
  s.plan.rename_folder(0, "Italy and Norway");
  s.plan.rename_folder(1, "Italy and Norway");
  auto const folders = s.plan.folders(s.context());
  REQUIRE(folders.size() == 1);
  CHECK(folders[0].name == "Italy and Norway");
  s.plan.rename_folder(0, "");
  CHECK(s.plan.folders(s.context())[0].name == "Italy and Norway");
}

TEST_CASE("the commit plan uses the back end's slug until the user renames a folder",
          "[trips_plan]") {
  scene s;
  s.plan.set_clustering({trip(0, {"a.jpg"}, "Rome, Italy · April 2026", "rome-italy-2026-04"),
                         trip(1, {"b.jpg"}, "Oslo, Norway · May 2026", "oslo-norway-2026-05")});
  auto plan = s.plan.commit_folder_plan(s.context());
  CHECK(plan.at("a.jpg") == "rome-italy-2026-04");
  CHECK(plan.at("b.jpg") == "oslo-norway-2026-05");
  CHECK_FALSE(plan.contains("c.jpg"));
  s.plan.rename_folder(0, "Italy Trip");
  CHECK(s.plan.commit_folder_plan(s.context()).at("a.jpg") == "italy-trip");
}

TEST_CASE("a trip with no slug is named from its folder label", "[trips_plan]") {
  scene s;
  s.plan.set_clustering({trip(0, {"a.jpg"}, "April 2026")});
  CHECK(s.plan.commit_folder_plan(s.context()).at("a.jpg") == "april-2026");
  s.plan.set_clustering({trip(0, {"a.jpg"}, "")});
  CHECK(s.plan.commit_folder_plan(s.context()).empty());
}

TEST_CASE("a trip with legs puts each photo in its leg's folder", "[trips_plan]") {
  scene s;
  auto multi = trip(0, {"a.jpg", "b.jpg", "c.jpg"}, "Trip", "italy-2026", "Italy");
  multi.legs = {{"Rome", "rome", {"a.jpg"}, std::nullopt},
                {"Florence", "", {"b.jpg"}, std::nullopt},
                {"Venice", "Venice!", {"c.jpg"}, std::nullopt}};
  s.plan.set_clustering({multi});
  auto const plan = s.plan.commit_folder_plan(s.context());
  CHECK(plan.at("a.jpg") == "italy-2026/rome");
  CHECK(plan.at("b.jpg") == "italy-2026/leg");
  CHECK(plan.at("c.jpg") == "italy-2026/venice");
}

TEST_CASE("the commit plan is empty when the user turns folders off", "[trips_plan]") {
  scene s;
  s.plan.set_organize_into_folders(false);
  CHECK(s.plan.commit_folder_plan(s.context()).empty());
}

TEST_CASE("edits describe the moves, renames and hand-made trips", "[trips_plan]") {
  scene s;
  s.plan.set_clustering({trip(0, {"a.jpg"}, "Rome"), trip(1, {"b.jpg"}, "Oslo")});
  s.plan.move_images({"b.jpg"}, 0);
  s.plan.rename_folder(0, "Italy and Norway");
  auto const edits = s.plan.edits();
  CHECK(edits.membership == std::map<std::string, std::int64_t>{{"b.jpg", 0}});
  REQUIRE(edits.renames.size() == 1);
  CHECK(edits.renames[0].trip_id == 0);
  CHECK(edits.renames[0].from == "Rome");
  CHECK(edits.renames[0].name == "Italy and Norway");
  CHECK(edits.organize_into_folders);
}

TEST_CASE("saved edits are laid over a new clustering and what no longer fits is dropped",
          "[trips_plan]") {
  scene s;
  s.plan.set_clustering({trip(0, {"a.jpg"}, "Rome"), trip(1, {"c.jpg"}, "Oslo")});
  trip_edits saved;
  saved.membership = {{"b.jpg", 0}, {"gone.jpg", 0}, {"a.jpg", 77}};
  saved.renames = {{0, "Rome", "Italy"}, {1, "Bergen", "Wrong"}};
  saved.organize_into_folders = false;
  s.plan.apply_edits(saved, s.images);

  CHECK(s.members(0) == std::vector<std::string>{"a.jpg", "b.jpg"});
  auto const folders = s.plan.folders(s.context());
  REQUIRE(folders.size() == 2);
  CHECK(folders[0].name == "Italy");
  CHECK(folders[1].name == "Oslo");
  CHECK_FALSE(s.plan.organize_into_folders());
}

TEST_CASE("saved edits bring back hand-made trips and keep their ids from being reused",
          "[trips_plan]") {
  scene s;
  trip_edits saved;
  saved.user_trips = {{1'000'004, 5000, "Camping", "Yosemite"}};
  saved.membership = {{"a.jpg", 1'000'004}};
  s.plan.apply_edits(saved, s.images);
  CHECK(s.members(1'000'004) == std::vector<std::string>{"a.jpg"});
  CHECK(s.plan.create_trip({"c.jpg"}, s.context(), ms(0)) == 1'000'005);
}

TEST_CASE("a new clustering drops the old edits", "[trips_plan]") {
  scene s;
  s.plan.move_images({"b.jpg"}, 1);
  s.plan.rename_folder(0, "X");
  s.plan.set_clustering({trip(0, {"a.jpg", "b.jpg"}, "Rome")});
  CHECK(s.plan.edits().membership.empty());
  CHECK(s.plan.edits().renames.empty());
}

TEST_CASE("slugify keeps lowercase letters and digits and joins the rest with hyphens",
          "[format]") {
  CHECK(slugify("Italy Trip") == "italy-trip");
  CHECK(slugify("  Rome, Italy · April 2026 ") == "rome-italy-april-2026");
  CHECK(slugify("---") == "");
  CHECK(slugify("a__b") == "a-b");
  CHECK(slugify("Zürich") == "z-rich");
}
