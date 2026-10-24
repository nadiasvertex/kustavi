#include "controller_fixture.h"

#include <algorithm>

using namespace kustavi::gui;
using namespace kustavi::gui::testing;

namespace {

auto commit_progress(std::uint32_t done, std::uint32_t total, std::string const &name)
    -> kustavi::CommitEvent {
  kustavi::CommitEvent event;
  event.mutable_progress()->set_done(done);
  event.mutable_progress()->set_total(total);
  event.mutable_progress()->set_current_name(name);
  return event;
}

auto commit_complete(std::uint32_t copied, std::uint32_t skipped = 0, std::uint32_t present = 0,
                     std::vector<std::string> const &errors = {}) -> kustavi::CommitEvent {
  kustavi::CommitEvent event;
  event.mutable_complete()->set_copied(copied);
  event.mutable_complete()->set_skipped(skipped);
  event.mutable_complete()->set_already_present(present);
  for (auto const &error : errors) {
    event.mutable_complete()->add_errors(error);
  }
  return event;
}

// The menu over a, b (Rome), c (Paris) and d (unassigned), with the sizes set, and the final review.
auto reach_deletion_review(harness &h) -> void {
  h.client->scan_events = {scan_image("a.jpg"), scan_image("b.jpg"), scan_image("c.jpg"),
                           scan_image("d.jpg")};
  for (auto &event : h.client->scan_events) {
    event.mutable_image()->set_size_bytes(1000);
  }
  h.client->scan_events.push_back(scan_complete(4));
  h.client->trips_events = {trip_event(1, 1000, {"a.jpg", "b.jpg"}, "Rome"),
                            trip_event(2, 2000, {"c.jpg"}, "Paris")};
  reach_confirm(h);
  h.controller.continue_from_confirm();
  h.settle();
  h.controller.continue_from_trips();
  h.controller.mark_image("d.jpg");
  h.controller.continue_from_batches();
  REQUIRE(h.in<wizard_deletion_review>());
}

auto reach_summary(harness &h) -> void {
  reach_deletion_review(h);
  h.controller.continue_from_deletion_review();
  h.settle();
  REQUIRE(h.in<wizard_commit_summary>());
}

} // namespace

TEST_CASE("the final review counts what is marked and groups it by batch", "[commit]") {
  harness h;
  reach_deletion_review(h);
  CHECK(h.get<wizard_deletion_review>().marked_count == 1);
  auto const groups = h.controller.marked_by_batch();
  REQUIRE(groups.size() == 1);
  CHECK(groups.front().title == "Unassigned");
  CHECK(groups.front().images.front().id == "d.jpg");
  CHECK(stage_index_of(h.controller.phase()) == static_cast<int>(wizard_stage::review));
}

TEST_CASE("a photo kept in the final review leaves the marked list", "[commit]") {
  harness h;
  reach_deletion_review(h);
  h.controller.keep_image("d.jpg");
  CHECK(h.get<wizard_deletion_review>().marked_count == 0);
  CHECK(h.controller.marked_by_batch().empty());
}

TEST_CASE("the final review goes back to the menu and on to the summary", "[commit]") {
  harness h;
  reach_deletion_review(h);
  h.controller.back_from_deletion_review();
  CHECK(h.in<wizard_batch_menu>());
  h.controller.continue_from_batches();
  h.controller.continue_from_deletion_review();
  h.settle();
  CHECK(h.in<wizard_commit_summary>());
}

TEST_CASE("the summary counts the kept photos and suggests a destination", "[commit]") {
  harness h;
  reach_summary(h);
  auto const &summary = h.get<wizard_commit_summary>();
  CHECK(summary.keep_count == 3);
  CHECK(summary.keep_bytes == 3000);
  CHECK(summary.left_behind_count == 1);
  CHECK(summary.destination == "/photos-kept");
  CHECK(stage_index_of(h.controller.phase()) == static_cast<int>(wizard_stage::copy));
}

TEST_CASE("the suggestion is a sibling of the source folder", "[commit]") {
  harness h;
  h.client->scan_events = {scan_image("a.jpg"), scan_complete(1)};
  reach_confirm(h, "/Users/me/Pictures/Italy 2024/");
  CHECK(h.controller.suggested_destination() == "/Users/me/Pictures/Italy 2024-kept");
}

TEST_CASE("the summary asks for an estimate and shows it for its destination", "[commit]") {
  harness h;
  h.client->estimate_response.set_fits(true);
  h.client->estimate_response.set_new_bytes(2000);
  reach_summary(h);
  auto const &summary = h.get<wizard_commit_summary>();
  REQUIRE(summary.estimate.has_value());
  CHECK(summary.space_confirmed());
  REQUIRE(h.client->last_estimate_request.has_value());
  CHECK(h.client->last_estimate_request->destination() == "/photos-kept");
  CHECK(h.client->last_estimate_request->merge_existing());
  CHECK(h.client->last_estimate_request->keep_ids_size() == 3);
}

TEST_CASE("an estimate that does not fit keeps the copy disabled", "[commit]") {
  harness h;
  h.client->estimate_response.set_fits(false);
  reach_summary(h);
  CHECK_FALSE(h.get<wizard_commit_summary>().space_confirmed());
}

TEST_CASE("an estimate that fails leaves the copy disabled", "[commit]") {
  harness h;
  h.client->unary_error = backend_rpc{grpc::StatusCode::INTERNAL, "no space info"};
  reach_deletion_review(h);
  h.controller.continue_from_deletion_review();
  h.settle();
  REQUIRE(h.in<wizard_commit_summary>());
  CHECK_FALSE(h.get<wizard_commit_summary>().estimate.has_value());
  CHECK_FALSE(h.get<wizard_commit_summary>().space_confirmed());
}

TEST_CASE("a new destination clears the estimate until the answer for it arrives",
          "[commit]") {
  harness h;
  h.client->estimate_response.set_fits(true);
  reach_summary(h);
  REQUIRE(h.get<wizard_commit_summary>().estimate.has_value());

  std::chrono::steady_clock::time_point now{};
  ui_timers timers{[&now] { return now; }};
  h.controller.set_timers(&timers);
  h.client->estimate_response.set_fits(false);
  h.controller.set_commit_destination("/elsewhere");
  // The old estimate was for another folder, and the new one waits for the typing to pause.
  CHECK(h.get<wizard_commit_summary>().destination == "/elsewhere");
  CHECK_FALSE(h.get<wizard_commit_summary>().estimate.has_value());
  CHECK(h.client->last_estimate_request->destination() == "/photos-kept");
  using namespace std::chrono_literals;
  now += 399ms;
  timers.run_due();
  h.settle();
  CHECK_FALSE(h.get<wizard_commit_summary>().estimate.has_value());
  now += 1ms;
  timers.run_due();
  h.settle();
  REQUIRE(h.get<wizard_commit_summary>().estimate.has_value());
  CHECK_FALSE(h.get<wizard_commit_summary>().space_confirmed());
  CHECK(h.client->last_estimate_request->destination() == "/elsewhere");
}

TEST_CASE("typing again restarts the pause", "[commit]") {
  harness h;
  reach_summary(h);
  std::chrono::steady_clock::time_point now{};
  ui_timers timers{[&now] { return now; }};
  h.controller.set_timers(&timers);
  using namespace std::chrono_literals;
  h.controller.set_commit_destination("/a");
  now += 300ms;
  h.controller.set_commit_destination("/ab");
  now += 300ms;
  timers.run_due();
  h.settle();
  CHECK(h.client->last_estimate_request->destination() == "/photos-kept");
  now += 100ms;
  timers.run_due();
  h.settle();
  CHECK(h.client->last_estimate_request->destination() == "/ab");
}

TEST_CASE("an empty destination asks for no estimate", "[commit]") {
  harness h;
  h.client->scan_events = {scan_image("a.jpg"), scan_complete(1)};
  reach_confirm(h, "/");
  h.controller.continue_from_confirm();
  h.settle();
  h.controller.continue_from_trips();
  h.controller.continue_from_batches();
  h.controller.continue_from_deletion_review();
  h.settle();
  REQUIRE(h.in<wizard_commit_summary>());
  CHECK(h.get<wizard_commit_summary>().destination.empty());
  CHECK_FALSE(h.client->last_estimate_request.has_value());
  h.controller.start_commit();
  CHECK(h.in<wizard_commit_summary>());
}

TEST_CASE("the summary goes back to the final review", "[commit]") {
  harness h;
  reach_summary(h);
  h.controller.back_from_commit_summary();
  CHECK(h.in<wizard_deletion_review>());
}

TEST_CASE("the copy runs with the kept photos and the folder plan", "[commit]") {
  harness h;
  reach_summary(h);
  h.client->commit_events = {commit_progress(1, 3, "a.jpg"), commit_progress(2, 3, "b.jpg"),
                             commit_complete(3, 0, 1, {"x.jpg: denied"})};
  h.controller.start_commit();
  h.settle();

  REQUIRE(h.client->last_commit_request.has_value());
  auto const &request = *h.client->last_commit_request;
  CHECK(request.destination() == "/photos-kept");
  CHECK(request.merge_existing());
  CHECK(request.keep_ids_size() == 3);
  CHECK(request.folder_for_id().at("a.jpg") == "Rome");
  CHECK(request.folder_for_id().at("c.jpg") == "Paris");

  REQUIRE(h.in<wizard_done>());
  auto const &done = h.get<wizard_done>();
  CHECK(done.copied_count == 3);
  CHECK(done.already_present_count == 1);
  CHECK(done.destination == "/photos-kept");
  CHECK(done.errors == std::vector<std::string>{"x.jpg: denied"});
  CHECK(stage_index_of(h.controller.phase()) == wizard_stage_count);
}

TEST_CASE("the copy shows its progress with bytes", "[commit]") {
  harness h;
  reach_summary(h);
  h.client->hold_streams = true;
  h.client->commit_events = {commit_progress(2, 3, "b.jpg")};
  h.controller.start_commit();
  h.settle();
  REQUIRE(h.in<wizard_committing>());
  CHECK(h.get<wizard_committing>().total == 3);
  CHECK(h.get<wizard_committing>().total_bytes == 3000);

  std::vector<wizard_committing> seen;
  h.controller.on_change([&] {
    if (auto const *running = std::get_if<wizard_committing>(&h.controller.phase())) {
      seen.push_back(*running);
    }
  });
  h.client->release_streams();
  h.queue.drain();
  REQUIRE_FALSE(seen.empty());
  CHECK(seen.front().done == 2);
  CHECK(seen.front().current_name == "b.jpg");
  CHECK(seen.front().done_bytes == 2000);
}

TEST_CASE("cancelling the copy returns to the summary", "[commit]") {
  harness h;
  reach_summary(h);
  h.client->hold_streams = true;
  h.controller.start_commit();
  h.settle();
  REQUIRE(h.in<wizard_committing>());
  h.controller.cancel_commit();
  h.settle();
  CHECK(h.in<wizard_commit_summary>());
  h.client->release_streams();
  h.settle();
  CHECK(h.in<wizard_commit_summary>());
}

TEST_CASE("a failing copy leads to the error screen", "[commit]") {
  harness h;
  reach_summary(h);
  h.client->stream_error = backend_rpc{grpc::StatusCode::FAILED_PRECONDITION, "disk full"};
  h.controller.start_commit();
  h.settle();
  REQUIRE(h.in<wizard_failed>());
  CHECK(h.get<wizard_failed>().step_index == index_of(wizard_step::copy));
}

TEST_CASE("start over after the copy clears everything", "[commit]") {
  harness h;
  reach_summary(h);
  h.controller.start_commit();
  h.settle();
  REQUIRE(h.in<wizard_done>());
  h.controller.reset_to_start();
  CHECK(h.in<wizard_start>());
  CHECK(h.controller.image_count() == 0);
  CHECK(h.controller.intent().explicit_deleted.empty());
}

TEST_CASE("a session saved at the copy step resumes on the summary", "[commit][resume]") {
  harness h;
  h.client->inspect_response.set_has_session(true);
  h.client->inspect_response.set_image_count(4);
  h.client->inspect_response.set_resume_step(6);
  h.client->scan_events = {scan_image("a.jpg"), scan_image("b.jpg"), scan_image("c.jpg"),
                           scan_image("d.jpg"), scan_complete(4)};
  h.client->trips_events = {trip_event(1, 1000, {"a.jpg", "b.jpg"}, "Rome"),
                            trip_event(2, 2000, {"c.jpg"}, "Paris")};
  h.client->session_results.set_resume_step(6);
  h.client->estimate_response.set_fits(true);
  h.controller.select_folder("/photos");
  h.settle();
  h.controller.resume_session();
  h.settle();
  REQUIRE(h.in<wizard_commit_summary>());
  CHECK(h.get<wizard_commit_summary>().space_confirmed());
}
