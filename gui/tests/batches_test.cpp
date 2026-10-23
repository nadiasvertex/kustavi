#include <catch2/catch_test_macros.hpp>

#include "core/batches.h"

using namespace kustavi::gui;

namespace {

auto trip_with(std::vector<std::string> members) -> trip_info {
  trip_info t;
  t.member_ids = std::move(members);
  return t;
}

} // namespace

TEST_CASE("each folder with photos becomes a batch in scan order, then the unassigned one",
          "[batches]") {
  std::vector<trip_folder_info> const folders{
      {"Paris", {trip_with({"c", "a"})}},
      {"", {trip_with({"b"})}},
      {"Empty", {trip_with({})}},
  };
  auto const batches = build_batches(folders, {"a", "b", "c", "d"}, {"d"});
  REQUIRE(batches.size() == 3);
  CHECK(batches[0].key == "Paris");
  CHECK(batches[0].image_ids == std::vector<std::string>{"a", "c"});
  CHECK(batches[1].key == "__no-folder__");
  CHECK(batches[1].title == "Other");
  CHECK(batches[2].key == "__unassigned__");
  CHECK(batches[2].title == "Unassigned");
  CHECK(batches[2].image_ids == std::vector<std::string>{"d"});
}

TEST_CASE("no unassigned photos means no unassigned batch", "[batches]") {
  CHECK(build_batches({{"Paris", {trip_with({"a"})}}}, {"a"}, {}).size() == 1);
  CHECK(build_batches({}, {"a"}, {}).empty());
}

TEST_CASE("a pass that ran over the whole session counts for every batch", "[batches]") {
  pass_progress progress;
  CHECK_FALSE(progress.done(wizard_step::quality, "Paris"));
  progress.session.insert(index_of(wizard_step::quality));
  CHECK(progress.done(wizard_step::quality, "Paris"));
  CHECK(progress.done(wizard_step::quality, "Rome"));
  CHECK_FALSE(progress.done(wizard_step::junk, "Paris"));
}

TEST_CASE("a pass that ran for one batch counts for that batch", "[batches]") {
  pass_progress progress;
  progress.mark_done(wizard_step::junk, "Paris");
  CHECK(progress.done(wizard_step::junk, "Paris"));
  CHECK_FALSE(progress.done(wizard_step::junk, "Rome"));
  CHECK(progress.batches.contains("3:Paris"));
}

TEST_CASE("a batch summary counts photos, videos and what each pass flagged", "[batches]") {
  batch_info const batch{"Paris", "Paris", {"a", "b", "v", "c"}};
  std::unordered_map<std::string, image_info> images;
  for (auto const *id : {"a", "b", "v", "c", "other"}) {
    image_info info;
    info.id = id;
    info.is_video = std::string{id} == "v";
    images[id] = info;
  }
  review_flags flags;
  flags.quality["a"] = {"a", {quality_reason::blurry}, 1, 0.5};
  flags.quality["other"] = {"other", {quality_reason::blurry}, 1, 0.5};
  flags.junk["b"] = {"b", "scan", 0.9};
  flags.video["v"] = {"v", "static", 0.9};
  flags.similar_groups = {{1, {"a", "x"}, "a", {}}, {2, {"y", "z"}, "y", {}}};
  pass_progress progress;
  progress.mark_done(wizard_step::quality, "Paris");

  auto const summary = summarize_batch(batch, images, flags, progress);
  CHECK(summary.photo_count == 3);
  CHECK(summary.video_count == 1);
  CHECK(summary.passes.at(index_of(wizard_step::quality)).flagged == 1);
  CHECK(summary.passes.at(index_of(wizard_step::quality)).done);
  CHECK(summary.passes.at(index_of(wizard_step::duplicates)).flagged == 1);
  CHECK_FALSE(summary.passes.at(index_of(wizard_step::duplicates)).done);
  CHECK(summary.passes.at(index_of(wizard_step::junk)).flagged == 1);
  CHECK(summary.passes.at(index_of(wizard_step::video)).flagged == 1);
  CHECK(summary.passes.size() == 4);
}
