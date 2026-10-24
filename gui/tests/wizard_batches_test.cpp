#include "controller_fixture.h"

#include <catch2/catch_approx.hpp>

#include <algorithm>

using namespace kustavi::gui;
using namespace kustavi::gui::testing;

namespace {

auto quality_flag(std::string const &id, kustavi::QualityReason reason = kustavi::BLURRY)
    -> kustavi::QualityEvent {
  kustavi::QualityEvent event;
  event.mutable_flag()->set_image_id(id);
  event.mutable_flag()->add_reasons(reason);
  event.mutable_flag()->set_sharpness(10);
  return event;
}

auto junk_flag(std::string const &id, double confidence = 0.9) -> kustavi::JunkEvent {
  kustavi::JunkEvent event;
  event.mutable_flag()->set_image_id(id);
  event.mutable_flag()->set_reason("screenshot");
  event.mutable_flag()->set_confidence(confidence);
  return event;
}

auto similar_group(std::uint32_t id, std::vector<std::string> const &members)
    -> kustavi::SimilarEvent {
  kustavi::SimilarEvent event;
  event.mutable_group()->set_id(id);
  for (auto const &member : members) {
    event.mutable_group()->add_image_ids(member);
  }
  event.mutable_group()->set_recommended_keep_id(members.front());
  return event;
}

auto video_flag(std::string const &id) -> kustavi::VideoEvent {
  kustavi::VideoEvent event;
  event.mutable_flag()->set_video_id(id);
  event.mutable_flag()->set_reason("too_short");
  event.mutable_flag()->set_confidence(0.8);
  return event;
}

// The batch menu over the four photos of `reach_trips_review`: Paris (c), Rome (a and b) and
// the unassigned d.
auto reach_menu(harness &h) -> void {
  reach_trips_review(h);
  h.controller.continue_from_trips();
  REQUIRE(h.in<wizard_batch_menu>());
}

auto batch_titles(harness &h) -> std::vector<std::string> {
  std::vector<std::string> titles;
  for (auto const &batch : h.get<wizard_batch_menu>().batches) {
    titles.push_back(batch.title);
  }
  return titles;
}

auto scope_of(google::protobuf::RepeatedPtrField<std::string> const &ids)
    -> std::vector<std::string> {
  return {ids.begin(), ids.end()};
}

} // namespace

TEST_CASE("the folders become the batches and the first one is selected", "[batches]") {
  harness h;
  reach_menu(h);
  CHECK(batch_titles(h) == std::vector<std::string>{"Paris", "Rome", "Unassigned"});
  auto const &menu = h.get<wizard_batch_menu>();
  CHECK(menu.selected_key == "Paris");
  CHECK(menu.total_images == 4);
  CHECK(menu.marked_count == 0);
  CHECK(menu.batches[1].photo_count == 2);
}

TEST_CASE("selecting a batch changes the selection", "[batches]") {
  harness h;
  reach_menu(h);
  h.controller.select_batch("Rome");
  CHECK(h.get<wizard_batch_menu>().selected_key == "Rome");
  h.controller.select_batch("nope");
  CHECK(h.get<wizard_batch_menu>().selected_key == "Rome");
}

TEST_CASE("a pass runs on the selected batch only and opens its review", "[batches]") {
  harness h;
  reach_menu(h);
  h.client->quality_events = {quality_flag("a.jpg")};
  h.controller.select_batch("Rome");
  h.controller.start_batch_pass(wizard_step::quality);
  h.settle();

  REQUIRE(h.client->last_quality_request.has_value());
  CHECK(scope_of(h.client->last_quality_request->scope_image_ids()) ==
        std::vector<std::string>{"a.jpg", "b.jpg"});
  CHECK(h.client->last_quality_request->batch_key() == "Rome");
  REQUIRE(h.in<wizard_quality_review>());
  CHECK(h.get<wizard_quality_review>().flagged_count == 1);
  CHECK(h.get<wizard_quality_review>().total_images == 2);

  h.controller.close_batch_review();
  REQUIRE(h.in<wizard_batch_menu>());
  auto const &rome = h.get<wizard_batch_menu>().batches[1];
  CHECK(rome.passes.at(index_of(wizard_step::quality)).done);
  CHECK(rome.passes.at(index_of(wizard_step::quality)).flagged == 1);
  CHECK_FALSE(h.get<wizard_batch_menu>().batches[0].passes.at(index_of(wizard_step::quality)).done);
}

TEST_CASE("a review counts only the flags of its batch", "[batches]") {
  harness h;
  reach_menu(h);
  h.client->quality_events = {quality_flag("a.jpg"), quality_flag("c.jpg")};
  h.controller.start_batch_pass(wizard_step::quality);
  h.settle();
  // The batch is Paris, so a flag outside it is not counted.
  CHECK(h.get<wizard_quality_review>().flagged_count == 1);
}

TEST_CASE("a finished pass can be reviewed again and run again", "[batches]") {
  harness h;
  reach_menu(h);
  h.client->quality_events = {quality_flag("c.jpg")};
  h.controller.start_batch_pass(wizard_step::quality);
  h.settle();
  h.controller.close_batch_review();

  h.controller.review_batch_pass(wizard_step::quality);
  CHECK(h.in<wizard_quality_review>());
  h.controller.close_batch_review();

  h.client->quality_events = {};
  h.controller.start_batch_pass(wizard_step::quality);
  h.settle();
  // The batch's old flags were dropped before the run.
  CHECK(h.get<wizard_quality_review>().flagged_count == 0);
}

TEST_CASE("passes run in any order", "[batches]") {
  harness h;
  reach_menu(h);
  h.client->similar_events = {similar_group(0, {"c.jpg"})};
  h.controller.start_batch_pass(wizard_step::duplicates);
  h.settle();
  CHECK(h.in<wizard_similar_review>());
  h.controller.close_batch_review();
  h.client->video_events = {video_flag("c.jpg")};
  h.controller.start_batch_pass(wizard_step::video);
  h.settle();
  REQUIRE(h.in<wizard_video_review>());
  CHECK(h.get<wizard_video_review>().flagged_count == 1);
  h.controller.close_batch_review();
  h.controller.start_batch_pass(wizard_step::quality);
  h.settle();
  CHECK(h.in<wizard_quality_review>());
}

TEST_CASE("running on all batches goes through every batch and ends at the menu", "[batches]") {
  harness h;
  reach_menu(h);
  h.client->quality_events = {quality_flag("a.jpg")};
  h.controller.start_pass_on_all_batches(wizard_step::quality);
  h.settle();
  REQUIRE(h.in<wizard_batch_menu>());
  for (auto const &batch : h.get<wizard_batch_menu>().batches) {
    CHECK(batch.passes.at(index_of(wizard_step::quality)).done);
  }
  CHECK(h.client->last_quality_request->batch_key() == std::string{unassigned_batch_key});
}

TEST_CASE("running on all batches skips the batches that are done", "[batches]") {
  harness h;
  reach_menu(h);
  h.controller.start_batch_pass(wizard_step::quality);
  h.settle();
  h.controller.close_batch_review();
  h.client->last_quality_request.reset();
  h.controller.start_pass_on_all_batches(wizard_step::quality);
  h.settle();
  CHECK(h.in<wizard_batch_menu>());
  CHECK(h.client->last_quality_request->batch_key() == std::string{unassigned_batch_key});
}

TEST_CASE("cancelling a running pass returns to the menu and drops late events", "[batches]") {
  harness h;
  reach_menu(h);
  h.client->hold_streams = true;
  h.client->quality_events = {quality_flag("c.jpg")};
  h.controller.start_batch_pass(wizard_step::quality);
  h.settle();
  REQUIRE(h.in<wizard_quality_running>());
  h.controller.cancel_pass();
  CHECK(h.in<wizard_batch_menu>());
  h.client->release_streams();
  h.settle();
  CHECK(h.in<wizard_batch_menu>());
  CHECK_FALSE(h.get<wizard_batch_menu>().batches[0].passes.at(index_of(wizard_step::quality)).done);
}

TEST_CASE("a failing pass keeps its results and the error screen leads back to the menu",
          "[batches]") {
  harness h;
  reach_menu(h);
  h.client->quality_events = {quality_flag("c.jpg")};
  h.controller.start_batch_pass(wizard_step::quality);
  h.settle();
  h.controller.close_batch_review();

  h.client->stream_error = backend_error{status_code::internal, "boom"};
  h.controller.start_batch_pass(wizard_step::duplicates);
  h.settle();
  REQUIRE(h.in<wizard_failed>());
  h.controller.go_back_from_error();
  REQUIRE(h.in<wizard_batch_menu>());
  CHECK(h.controller.flags().quality.contains("c.jpg"));
}

TEST_CASE("the junk pass waits for the model and starts when it is ready", "[batches][junk]") {
  harness h;
  reach_menu(h);
  h.client->junk_events = {junk_flag("c.jpg")};
  h.controller.start_batch_pass(wizard_step::junk);
  h.settle();
  REQUIRE(h.in<wizard_junk_prep>());
  CHECK_FALSE(h.client->last_junk_request.has_value());

  h.controller.set_model_ready(true);
  h.settle();
  REQUIRE(h.in<wizard_junk_review>());
  CHECK(h.get<wizard_junk_review>().flagged_count == 1);
}

TEST_CASE("the junk pass starts at once when the model is ready", "[batches][junk]") {
  harness h;
  reach_menu(h);
  h.controller.set_model_ready(true);
  h.controller.start_batch_pass(wizard_step::junk);
  h.settle();
  CHECK(h.in<wizard_junk_review>());
}

TEST_CASE("cancelling the junk preparation stops the download and returns to the menu",
          "[batches][junk]") {
  harness h;
  reach_menu(h);
  int cancelled = 0;
  h.controller.set_cancel_model_download([&] { ++cancelled; });
  h.controller.start_batch_pass(wizard_step::junk);
  h.settle();
  h.controller.cancel_junk_prep();
  CHECK(cancelled == 1);
  CHECK(h.in<wizard_batch_menu>());
}

TEST_CASE("the junk pass skips photos that an earlier step marked", "[batches][junk]") {
  harness h;
  reach_menu(h);
  h.client->quality_events = {quality_flag("c.jpg")};
  h.controller.start_batch_pass(wizard_step::quality);
  h.settle();
  h.controller.close_batch_review();
  h.controller.set_model_ready(true);
  h.controller.start_batch_pass(wizard_step::junk);
  h.settle();
  auto const skipped = scope_of(h.client->last_junk_request->skip_image_ids());
  CHECK(skipped == std::vector<std::string>{"c.jpg"});
}

TEST_CASE("the duplicate and video passes skip what the earlier steps marked", "[batches]") {
  harness h;
  reach_menu(h);
  h.client->quality_events = {quality_flag("c.jpg")};
  h.controller.start_batch_pass(wizard_step::quality);
  h.settle();
  h.controller.close_batch_review();
  h.controller.start_batch_pass(wizard_step::duplicates);
  h.settle();
  CHECK(scope_of(h.client->last_similar_request->skip_image_ids()) ==
        std::vector<std::string>{"c.jpg"});
  h.controller.close_batch_review();
  h.controller.start_batch_pass(wizard_step::video);
  h.settle();
  CHECK(scope_of(h.client->last_video_request->skip_video_ids()) ==
        std::vector<std::string>{"c.jpg"});
}

TEST_CASE("the junk pass measures its speed after the first real gap", "[batches][junk]") {
  harness h;
  reach_menu(h);
  h.controller.set_model_ready(true);
  // Each look at the clock is a second later, so every progress event follows a real gap.
  auto ticks = std::make_shared<std::int64_t>(0);
  h.controller.set_clock([ticks] {
    return time_ms{std::chrono::milliseconds{(*ticks)++ * 1000}};
  });
  std::vector<wizard_junk_running> seen;
  h.controller.on_change([&] {
    if (auto const *running = std::get_if<wizard_junk_running>(&h.controller.phase())) {
      seen.push_back(*running);
    }
  });
  auto const progress = [](std::uint32_t done) {
    kustavi::JunkEvent event;
    event.mutable_progress()->set_done(done);
    event.mutable_progress()->set_total(100);
    return event;
  };
  h.client->junk_events = {progress(10), progress(11), progress(13)};
  h.controller.start_batch_pass(wizard_step::junk);
  h.settle();

  REQUIRE(seen.size() >= 4);
  // The first event only starts the clock. The second starts the profile at the first one, so the
  // estimate needs a third.
  CHECK_FALSE(seen[1].seconds_per_image.has_value());
  auto const &last = seen.back();
  REQUIRE(last.seconds_per_image.has_value());
  // Three photos in the two seconds since the profile started.
  CHECK(*last.seconds_per_image == Catch::Approx(2.0 / 3.0));
  REQUIRE(last.estimated_completion.has_value());
  CHECK(last.done == 13);
}

TEST_CASE("marking and keeping change the counts and are saved", "[batches][deletion]") {
  harness h;
  reach_menu(h);
  h.client->quality_events = {quality_flag("c.jpg")};
  h.controller.start_batch_pass(wizard_step::quality);
  h.settle();
  h.client->saved_session_states.clear();

  CHECK(h.controller.is_marked("c.jpg", deletion_step::quality));
  h.controller.keep_image("c.jpg");
  CHECK_FALSE(h.controller.is_marked("c.jpg", deletion_step::quality));
  REQUIRE_FALSE(h.client->saved_session_states.empty());
  auto const &saved = h.client->saved_session_states.back();
  CHECK(saved.replace_decisions());
  REQUIRE(saved.decisions_size() == 1);
  CHECK(saved.decisions(0).decision() == kustavi::KEEP);

  h.controller.mark_image("c.jpg");
  CHECK(h.controller.is_marked("c.jpg", deletion_step::quality));
  CHECK(h.controller.deleted_ids() == std::vector<std::string>{"c.jpg"});
  h.controller.close_batch_review();
  CHECK(h.get<wizard_batch_menu>().marked_count == 1);
}

TEST_CASE("keep all and mark all act on the flagged photos of the batch", "[batches][deletion]") {
  harness h;
  reach_menu(h);
  h.client->quality_events = {quality_flag("a.jpg"), quality_flag("b.jpg"), quality_flag("c.jpg")};
  h.controller.select_batch("Rome");
  h.controller.start_batch_pass(wizard_step::quality);
  h.settle();
  h.controller.keep_all_flagged(deletion_step::quality);
  CHECK(h.controller.intent().explicit_kept ==
        std::set<std::string>{"a.jpg", "b.jpg"});
  h.controller.mark_all_flagged(deletion_step::quality);
  CHECK(h.controller.intent().explicit_deleted ==
        std::set<std::string>{"a.jpg", "b.jpg"});
}

TEST_CASE("a duplicate group's keeper can be chosen, and the other members are marked",
          "[batches][deletion]") {
  harness h;
  reach_menu(h);
  h.client->similar_events = {similar_group(0, {"a.jpg", "b.jpg"})};
  h.controller.select_batch("Rome");
  h.controller.start_batch_pass(wizard_step::duplicates);
  h.settle();
  REQUIRE(h.in<wizard_similar_review>());
  CHECK(h.get<wizard_similar_review>().group_count == 1);
  CHECK(h.get<wizard_similar_review>().marked_count == 1);
  CHECK(h.controller.is_marked("b.jpg", deletion_step::similar));
  h.controller.set_group_keeper(0, "b.jpg");
  CHECK(h.controller.is_marked("a.jpg", deletion_step::similar));
  CHECK_FALSE(h.controller.is_marked("b.jpg", deletion_step::similar));
  CHECK(h.client->saved_session_states.back().group_keepers().at(0) == "b.jpg");
}

TEST_CASE("marked photos leave their trips and are explained", "[batches][deletion]") {
  harness h;
  reach_menu(h);
  h.client->quality_events = {quality_flag("c.jpg")};
  h.controller.start_batch_pass(wizard_step::quality);
  h.settle();
  h.controller.close_batch_review();
  h.controller.mark_image("d.jpg");
  auto const reasons = h.controller.reasons_for("c.jpg");
  CHECK(reasons == std::vector<std::string>{"Blurry"});
  CHECK(h.controller.reasons_for("d.jpg") == std::vector<std::string>{"Marked by you"});

  h.controller.reopen_organize();
  REQUIRE(h.in<wizard_trips_review>());
  CHECK(h.get<wizard_trips_review>().unassigned_ids.empty());
  CHECK(h.get<wizard_trips_review>().marked_count == 1);
}

TEST_CASE("the thresholds preview, rerun and reset", "[batches][quality]") {
  harness h;
  reach_menu(h);
  h.client->quality_events = {quality_flag("c.jpg")};
  h.controller.start_batch_pass(wizard_step::quality);
  h.settle();
  REQUIRE(h.in<wizard_quality_review>());
  CHECK_FALSE(h.get<wizard_quality_review>().rerun_enabled);
  CHECK_FALSE(h.get<wizard_quality_review>().preview_flagged.has_value());

  h.client->preview_response.set_flagged(7);
  h.controller.set_blur_threshold(150);
  h.settle();
  auto const &review = h.get<wizard_quality_review>();
  CHECK(review.rerun_enabled);
  CHECK(review.preview_flagged == 7);
  CHECK(h.client->last_preview_request->thresholds().blur_threshold() == 150);
  CHECK(scope_of(h.client->last_preview_request->scope_image_ids()) ==
        std::vector<std::string>{"c.jpg"});

  h.controller.rerun_quality_pass();
  h.settle();
  CHECK(h.client->last_quality_request->blur_threshold() == 150);
  REQUIRE(h.in<wizard_quality_review>());
  CHECK_FALSE(h.get<wizard_quality_review>().rerun_enabled);
  CHECK_FALSE(h.get<wizard_quality_review>().preview_flagged.has_value());

  h.controller.reset_thresholds();
  CHECK(h.controller.tunables().blur_threshold == 100);
  CHECK(h.get<wizard_quality_review>().rerun_enabled);
}

TEST_CASE("the rerun does nothing without a change", "[batches][quality]") {
  harness h;
  reach_menu(h);
  h.controller.start_batch_pass(wizard_step::quality);
  h.settle();
  h.client->last_quality_request.reset();
  h.controller.rerun_quality_pass();
  CHECK_FALSE(h.client->last_quality_request.has_value());
}

TEST_CASE("the review changes do not save the position on each slider step", "[batches][quality]") {
  harness h;
  reach_menu(h);
  h.controller.start_batch_pass(wizard_step::quality);
  h.settle();
  auto const saved = h.client->saved_session_states.size();
  h.controller.set_blur_threshold(120);
  h.controller.set_blur_threshold(130);
  h.settle();
  CHECK(h.client->saved_session_states.size() == saved);
}

TEST_CASE("the menu goes back to the trips and forward again with new batches", "[batches]") {
  harness h;
  reach_menu(h);
  h.client->quality_events = {};
  h.controller.start_batch_pass(wizard_step::quality);
  h.settle();
  h.controller.close_batch_review();

  h.controller.reopen_organize();
  REQUIRE(h.in<wizard_trips_review>());
  h.controller.move_images_to_trip({"d.jpg"}, 1);
  h.controller.continue_from_trips();
  REQUIRE(h.in<wizard_batch_menu>());
  CHECK(batch_titles(h) == std::vector<std::string>{"Paris", "Rome"});
  // The progress of the old batches is gone.
  CHECK_FALSE(h.get<wizard_batch_menu>().batches[0].passes.at(index_of(wizard_step::quality)).done);
}

TEST_CASE("a resumed session restores the flags, the choices and the finished passes",
          "[batches][resume]") {
  harness h;
  h.client->inspect_response.set_has_session(true);
  h.client->inspect_response.set_image_count(4);
  h.client->inspect_response.set_resume_step(1);
  h.client->scan_events = {scan_image("a.jpg"), scan_image("b.jpg"), scan_image("c.jpg"),
                           scan_image("d.jpg"), scan_complete(4)};
  h.client->trips_events = {trip_event(1, 1000, {"a.jpg", "b.jpg"}, "Rome"),
                            trip_event(2, 2000, {"c.jpg"}, "Paris")};
  auto &results = h.client->session_results;
  results.set_resume_step(1);
  results.add_quality_flags()->set_image_id("a.jpg");
  results.mutable_quality_flags(0)->add_reasons(kustavi::BLURRY);
  auto *group = results.add_similar_groups();
  group->set_id(0);
  group->add_image_ids("c.jpg");
  group->set_recommended_keep_id("c.jpg");
  results.add_junk_flags()->set_image_id("d.jpg");
  auto *kept = results.add_decisions();
  kept->set_image_id("a.jpg");
  kept->set_decision(kustavi::KEEP);
  auto *deleted = results.add_decisions();
  deleted->set_image_id("b.jpg");
  deleted->set_decision(kustavi::DELETE);
  results.add_completed_batch_passes("1:Rome");
  results.set_similar_done(true);

  h.controller.select_folder("/photos");
  h.settle();
  h.controller.resume_session();
  h.settle();

  REQUIRE(h.in<wizard_batch_menu>());
  auto const &flags = h.controller.flags();
  CHECK(flags.quality.contains("a.jpg"));
  CHECK(flags.junk.contains("d.jpg"));
  CHECK(flags.similar_groups.size() == 1);
  CHECK(h.controller.is_marked("b.jpg", std::nullopt));
  CHECK_FALSE(h.controller.is_marked("a.jpg", deletion_step::quality));
  auto const &menu = h.get<wizard_batch_menu>();
  // b was marked by hand and d was flagged as junk.
  CHECK(menu.marked_count == 2);
  // Rome finished the quality pass, and a pass that finished over the session counts for each.
  CHECK(menu.batches[1].passes.at(index_of(wizard_step::quality)).done);
  CHECK(menu.batches[0].passes.at(index_of(wizard_step::duplicates)).done);
  CHECK_FALSE(menu.batches[0].passes.at(index_of(wizard_step::quality)).done);
  // The restore did not write the decisions back.
  CHECK(std::ranges::none_of(h.client->saved_session_states,
                             [](auto const &saved) { return saved.replace_decisions(); }));
}
