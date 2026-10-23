#include <catch2/catch_test_macros.hpp>

#include "core/deletion.h"

using namespace kustavi::gui;

namespace {

auto quality_flags(std::initializer_list<std::string> ids) -> flag_sets {
  flag_sets sets;
  sets.quality.insert(ids.begin(), ids.end());
  return sets;
}

auto flag(std::string id, quality_reason reason = quality_reason::blurry) -> quality_flag_info {
  return {std::move(id), {reason}, 5.0, 0.5};
}

auto image(std::string id) -> image_info {
  image_info info;
  info.id = id;
  info.name = id;
  return info;
}

} // namespace

TEST_CASE("explicit deletion beats every rule", "[deletion]") {
  deletion_intent plan;
  plan.explicit_deleted = {"a.jpg"};
  CHECK(is_marked_for_deletion(plan, "a.jpg", deletion_step::quality, {}));
  CHECK(is_marked_for_deletion(plan, "a.jpg", std::nullopt, {}));
}

TEST_CASE("an explicit keep beats the step default", "[deletion]") {
  deletion_intent plan;
  plan.explicit_kept = {"a.jpg"};
  CHECK_FALSE(is_marked_for_deletion(plan, "a.jpg", deletion_step::quality,
                                     quality_flags({"a.jpg"})));
}

TEST_CASE("an explicit delete beats an explicit keep", "[deletion]") {
  deletion_intent plan;
  plan.explicit_deleted = {"a.jpg"};
  plan.explicit_kept = {"a.jpg"};
  CHECK(is_marked_for_deletion(plan, "a.jpg", std::nullopt, {}));
}

TEST_CASE("the quality default marks flagged images only", "[deletion]") {
  deletion_intent const plan;
  auto const flags = quality_flags({"a.jpg"});
  CHECK(is_marked_for_deletion(plan, "a.jpg", deletion_step::quality, flags));
  CHECK_FALSE(is_marked_for_deletion(plan, "b.jpg", deletion_step::quality, flags));
}

TEST_CASE("the junk and video defaults mark their own flags", "[deletion]") {
  deletion_intent const plan;
  flag_sets flags;
  flags.junk = {"j.jpg"};
  flags.video = {"v.mp4"};
  CHECK(is_marked_for_deletion(plan, "j.jpg", deletion_step::junk, flags));
  CHECK_FALSE(is_marked_for_deletion(plan, "j.jpg", deletion_step::video, flags));
  CHECK(is_marked_for_deletion(plan, "v.mp4", deletion_step::video, flags));
  CHECK_FALSE(is_marked_for_deletion(plan, "v.mp4", deletion_step::junk, flags));
}

TEST_CASE("the similar default marks everything but the keeper", "[deletion]") {
  deletion_intent const plan;
  flag_sets flags;
  flags.similar_keepers = {{"a.jpg", "b.jpg"}, {"b.jpg", "b.jpg"}, {"c.jpg", ""}};
  CHECK(is_marked_for_deletion(plan, "a.jpg", deletion_step::similar, flags));
  CHECK_FALSE(is_marked_for_deletion(plan, "b.jpg", deletion_step::similar, flags));
  // An empty keeper means that the group has no recommendation.
  CHECK_FALSE(is_marked_for_deletion(plan, "c.jpg", deletion_step::similar, flags));
  CHECK_FALSE(is_marked_for_deletion(plan, "d.jpg", deletion_step::similar, flags));
}

TEST_CASE("without a step only the explicit rules apply", "[deletion]") {
  deletion_intent const plan;
  CHECK_FALSE(is_marked_for_deletion(plan, "a.jpg", std::nullopt, quality_flags({"a.jpg"})));
}

TEST_CASE("toggling a photo that is not marked marks it", "[deletion]") {
  auto const marked = deletion_intent{}.with_toggle("a.jpg");
  CHECK(marked.explicit_deleted.contains("a.jpg"));
  CHECK(marked.explicit_kept.empty());
}

TEST_CASE("toggling a marked photo keeps it", "[deletion]") {
  deletion_intent plan;
  plan.explicit_deleted = {"a.jpg"};
  auto const kept = plan.with_toggle("a.jpg");
  CHECK(kept.explicit_kept.contains("a.jpg"));
  CHECK(kept.explicit_deleted.empty());
}

TEST_CASE("keep all clears the marks on the photos it names", "[deletion]") {
  deletion_intent plan;
  plan.explicit_deleted = {"a.jpg", "b.jpg"};
  auto const kept = plan.with_keep_all({"a.jpg"});
  CHECK(kept.explicit_deleted.contains("b.jpg"));
  CHECK_FALSE(kept.explicit_deleted.contains("a.jpg"));
  CHECK(kept.explicit_kept.contains("a.jpg"));
}

TEST_CASE("mark all removes the keeps on the photos it names", "[deletion]") {
  deletion_intent plan;
  plan.explicit_kept = {"a.jpg", "b.jpg"};
  auto const marked = plan.with_mark_all({"b.jpg"});
  CHECK(marked.explicit_kept.contains("a.jpg"));
  CHECK_FALSE(marked.explicit_kept.contains("b.jpg"));
  CHECK(marked.explicit_deleted.contains("b.jpg"));
}

TEST_CASE("a change leaves the plan it came from alone", "[deletion]") {
  deletion_intent const plan;
  auto const marked = plan.with_marked("a.jpg");
  CHECK(plan.explicit_deleted.empty());
  CHECK(marked.explicit_deleted.size() == 1);
}

TEST_CASE("a keeper choice belongs to its group", "[deletion]") {
  auto const plan = deletion_intent{}.with_keeper(1, "b.jpg");
  CHECK(plan.user_keeper_of(1) == "b.jpg");
  CHECK_FALSE(plan.user_keeper_of(2).has_value());
  CHECK(plan.with_toggle("x.jpg").user_keeper_of(1) == "b.jpg");
}

TEST_CASE("the keeper map falls back to the recommendation", "[deletion]") {
  std::vector<similar_group_info> const groups{{1, {"a.jpg", "b.jpg"}, "a.jpg", {0.93, 0.81}}};
  using map = std::unordered_map<std::string, std::string>;
  CHECK(similar_keeper_map({}, groups) == map{{"a.jpg", "a.jpg"}, {"b.jpg", "a.jpg"}});
  auto const plan = deletion_intent{}.with_keeper(1, "b.jpg");
  CHECK(similar_keeper_map(plan, groups) == map{{"a.jpg", "b.jpg"}, {"b.jpg", "b.jpg"}});
}

namespace {

// Three photos in one group where `b` is the recommended keeper, a blurry photo, a junk photo and
// a flagged video.
struct scene {
  scene() {
    ids = {"a", "b", "c", "blur", "junk", "vid", "plain"};
    flags.quality["blur"] = flag("blur");
    flags.junk["junk"] = {"junk", "screenshot", 0.9};
    flags.video["vid"] = {"vid", "too_short", 0.8};
    flags.similar_groups = {{1, {"b", "a", "c"}, "b", {0.9, 0.8, 0.7}}};
  }

  std::vector<std::string> ids;
  review_flags flags;
};

} // namespace

TEST_CASE("deleted photos are those any step or the user marks, in scan order", "[deletion]") {
  scene s;
  using ids = std::vector<std::string>;
  CHECK(deleted_image_ids({}, s.ids, s.flags) == ids{"a", "c", "blur", "junk", "vid"});
  auto const plan = deletion_intent{}.with_marked("plain").with_kept("a").with_kept("vid");
  CHECK(deleted_image_ids(plan, s.ids, s.flags) == ids{"c", "blur", "junk", "plain"});
}

TEST_CASE("each pass skips what the passes before it already deleted", "[deletion]") {
  scene s;
  using ids = std::vector<std::string>;
  // The duplicate pass runs after quality, so only quality flags count and no group exists yet.
  CHECK(deleted_before_similar({}, s.ids, s.flags) == ids{"blur"});
  // The junk pass runs after quality and duplicates.
  CHECK(deleted_before_junk({}, s.ids, s.flags) == ids{"a", "c", "blur"});
  // The video pass runs after every other pass.
  CHECK(deleted_before_video({}, s.ids, s.flags) == ids{"a", "c", "blur", "junk"});
}

TEST_CASE("an explicit keep spares a photo from the earlier steps too", "[deletion]") {
  scene s;
  auto const plan = deletion_intent{}.with_kept("blur");
  CHECK(deleted_before_similar(plan, s.ids, s.flags).empty());
}

TEST_CASE("an explicit mark counts for every pass", "[deletion]") {
  scene s;
  auto const plan = deletion_intent{}.with_marked("plain");
  CHECK(deleted_before_similar(plan, s.ids, s.flags) ==
        std::vector<std::string>{"blur", "plain"});
}

TEST_CASE("the keep list is the scan order without the deleted photos", "[deletion]") {
  CHECK(keep_ids({"a", "b", "c", "d"}, {"b", "d"}) == std::vector<std::string>{"a", "c"});
  CHECK(keep_ids({"a", "b"}, {}) == std::vector<std::string>{"a", "b"});
  CHECK(keep_ids({}, {"a"}).empty());
}

TEST_CASE("deletion reasons name each pass that flagged a photo", "[deletion]") {
  scene s;
  using reasons = std::vector<std::string>;
  CHECK(deletion_reasons("blur", {}, s.flags) == reasons{"Blurry"});
  CHECK(deletion_reasons("junk", {}, s.flags) == reasons{"screenshot"});
  CHECK(deletion_reasons("vid", {}, s.flags) == reasons{"Too short"});
  CHECK(deletion_reasons("a", {}, s.flags) == reasons{"Duplicate"});
  CHECK(deletion_reasons("b", {}, s.flags).empty());
  CHECK(deletion_reasons("plain", {}, s.flags).empty());
}

TEST_CASE("a photo the user marked also says so", "[deletion]") {
  scene s;
  auto const plan = deletion_intent{}.with_marked("blur");
  CHECK(deletion_reasons("blur", plan, s.flags) == std::vector<std::string>{"Blurry", "Marked by you"});
  CHECK(deletion_reasons("plain", deletion_intent{}.with_marked("plain"), s.flags) ==
        std::vector<std::string>{"Marked by you"});
}

TEST_CASE("quality reasons use the review labels", "[deletion]") {
  review_flags flags;
  flags.quality["x"] = {"x",
                        {quality_reason::blurry, quality_reason::under_exposed,
                         quality_reason::over_exposed},
                        1, 0.1};
  CHECK(deletion_reasons("x", {}, flags) ==
        std::vector<std::string>{"Blurry", "Underexposed", "Overexposed"});
}

TEST_CASE("a reassigned keeper changes which duplicates are deleted", "[deletion]") {
  scene s;
  auto const plan = deletion_intent{}.with_keeper(1, "a");
  using ids = std::vector<std::string>;
  CHECK(deleted_image_ids(plan, s.ids, s.flags) == ids{"b", "c", "blur", "junk", "vid"});
}

TEST_CASE("marked photos are grouped by batch, leaving out empty batches", "[deletion]") {
  std::vector<batch_info> const batches{{"k1", "Paris", {"a", "b"}},
                                        {"k2", "Rome", {"c"}},
                                        {"k3", "Unassigned", {"d", "missing"}}};
  std::unordered_map<std::string, image_info> images;
  for (auto const *id : {"a", "b", "c", "d"}) {
    images[id] = image(id);
  }
  auto const groups = marked_by_batch(batches, images, {"b", "d", "missing"});
  REQUIRE(groups.size() == 2);
  CHECK(groups[0].title == "Paris");
  REQUIRE(groups[0].images.size() == 1);
  CHECK(groups[0].images[0].id == "b");
  CHECK(groups[1].title == "Unassigned");
  CHECK(groups[1].images.size() == 1);
}
