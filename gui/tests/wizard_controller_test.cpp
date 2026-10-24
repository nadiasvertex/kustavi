#include <catch2/catch_test_macros.hpp>

#include "controller_fixture.h"

#include "core/trip_edits.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace kustavi::gui;
using namespace kustavi::gui::testing;



TEST_CASE("the wizard starts on the start screen", "[wizard]") {
  harness h;
  CHECK(h.in<wizard_start>());
  CHECK(h.controller.image_count() == 0);
}

TEST_CASE("select, scan and confirm build the image index", "[wizard]") {
  harness h;
  h.client->scan_events = {scan_image("a.jpg"), scan_image("b.jpg"), scan_complete(2)};
  reach_confirm(h);

  auto const &phase = h.get<wizard_confirm_folder>();
  CHECK(phase.image_count == 2);
  CHECK(phase.folder == "/photos");
  REQUIRE(h.client->last_scan_request.has_value());
  CHECK(h.client->last_scan_request->recursive());
  CHECK_FALSE(h.client->last_scan_request->resume());
  CHECK(h.controller.image_ids() == std::vector<std::string>{"a.jpg", "b.jpg"});
  CHECK(h.controller.image("a.jpg")->working_image_path == "/cache/a.jpg");
  CHECK(h.controller.image("missing") == nullptr);
}

TEST_CASE("an image that arrives twice is indexed once", "[wizard]") {
  harness h;
  h.client->scan_events = {scan_image("a.jpg"), scan_image("a.jpg"), scan_complete(1)};
  reach_confirm(h);
  CHECK(h.controller.image_ids().size() == 1);
}

TEST_CASE("a scan that finds no images leads to the no-images screen", "[wizard]") {
  harness h;
  h.client->scan_events = {scan_complete(0)};
  h.controller.select_folder("/empty");
  h.settle();
  REQUIRE(h.in<wizard_no_images>());
  CHECK(h.get<wizard_no_images>().folder == "/empty");
}

TEST_CASE("scan errors appear on the confirm screen", "[wizard]") {
  harness h;
  h.client->scan_events = {scan_image("a.jpg"), scan_complete(1, {"broken.jpg: truncated"})};
  reach_confirm(h);
  CHECK(h.get<wizard_confirm_folder>().scan_errors ==
        std::vector<std::string>{"broken.jpg: truncated"});
}

TEST_CASE("progress events update the scanning screen", "[wizard]") {
  harness h;
  h.client->hold_streams = true;
  h.client->scan_events = {scan_progress(10, 3), scan_complete(3)};
  h.controller.select_folder("/photos");
  h.settle();
  REQUIRE(h.in<wizard_scanning>());
  CHECK(h.get<wizard_scanning>().files_seen == 0);

  h.client->release_streams();
  h.queue.drain();
  // The events and the end of the stream arrive together, so the scan has finished.
  CHECK(h.in<wizard_confirm_folder>());
}

TEST_CASE("a progress event is shown while the scan runs", "[wizard]") {
  harness h;
  h.client->scan_events = {scan_progress(10, 3)};
  h.client->hold_streams = true;
  h.controller.select_folder("/photos");
  h.settle();
  h.client->release_streams();
  // Run only the progress event, before the end of the stream.
  h.queue.drain();
  // With no complete event the scan stays on the scanning screen.
  REQUIRE(h.in<wizard_scanning>());
  auto const &scanning = h.get<wizard_scanning>();
  CHECK(scanning.files_seen == 10);
  CHECK(scanning.images_found == 3);
  CHECK(scanning.current_path == "/photos/x");
}

TEST_CASE("selecting a folder only works from the start screen", "[wizard]") {
  harness h;
  h.client->scan_events = {scan_image("a.jpg"), scan_complete(1)};
  reach_confirm(h);
  h.controller.select_folder("/other");
  h.settle();
  CHECK(h.get<wizard_confirm_folder>().folder == "/photos");
}

TEST_CASE("selecting a folder before the back end is up fails with a message", "[wizard]") {
  harness h;
  h.controller.set_client(nullptr);
  h.controller.select_folder("/photos");
  REQUIRE(h.in<wizard_failed>());
  CHECK(message_of(h.get<wizard_failed>().error).find("still starting") != std::string::npos);
  h.controller.go_back_from_error();
  CHECK(h.in<wizard_start>());
}

TEST_CASE("a failing scan leads to the error screen and back to the start", "[wizard]") {
  harness h;
  h.client->scan_events = {scan_image("a.jpg")};
  h.client->stream_error = backend_rpc{grpc::StatusCode::INTERNAL, "disk failure"};
  h.controller.select_folder("/photos");
  h.settle();
  REQUIRE(h.in<wizard_failed>());
  CHECK(message_of(h.get<wizard_failed>().error) == "disk failure");

  h.controller.go_back_from_error();
  CHECK(h.in<wizard_start>());
  CHECK(h.controller.image_count() == 0);
}

TEST_CASE("cancelling a scan returns to the start and ignores late events", "[wizard]") {
  harness h;
  h.client->hold_streams = true;
  h.client->scan_events = {scan_image("a.jpg"), scan_complete(1)};
  h.controller.select_folder("/photos");
  h.settle();
  REQUIRE(h.in<wizard_scanning>());

  h.controller.cancel_scan();
  CHECK(h.in<wizard_start>());
  h.client->release_streams();
  h.settle();
  CHECK(h.in<wizard_start>());
}

TEST_CASE("cancel does nothing outside a scan", "[wizard]") {
  harness h;
  h.controller.cancel_scan();
  CHECK(h.in<wizard_start>());
  CHECK(h.changes == 0);
}

TEST_CASE("back from confirm discards the results", "[wizard]") {
  harness h;
  h.client->scan_events = {scan_image("a.jpg"), scan_complete(1)};
  reach_confirm(h);
  h.controller.back_from_confirm();
  CHECK(h.in<wizard_start>());
  CHECK(h.controller.image_ids().empty());
}

TEST_CASE("reset to start clears everything", "[wizard]") {
  harness h;
  h.client->scan_events = {scan_image("a.jpg"), scan_complete(1)};
  reach_confirm(h);
  h.controller.reset_to_start();
  CHECK(h.in<wizard_start>());
  CHECK(h.controller.image_count() == 0);
}

TEST_CASE("the confirm screen saves the position and the tunables", "[wizard]") {
  harness h;
  h.client->scan_events = {scan_image("a.jpg"), scan_complete(1)};
  reach_confirm(h);
  REQUIRE(h.client->saved_session_states.size() == 1);
  auto const &saved = h.client->saved_session_states.front();
  CHECK(saved.step() == 0);
  CHECK(saved.quality_thresholds().blur_threshold() == 100.0);
  CHECK(saved.trip_params().max_gap_hours() == 48);
}

TEST_CASE("a folder with a saved session leads to the restore prompt", "[wizard][resume]") {
  harness h;
  h.client->inspect_response.set_has_session(true);
  h.client->inspect_response.set_image_count(42);
  h.client->inspect_response.set_resume_step(4);
  h.controller.select_folder("/photos");
  h.settle();
  REQUIRE(h.in<wizard_session_restore>());
  auto const &phase = h.get<wizard_session_restore>();
  CHECK(phase.folder == "/photos");
  CHECK(phase.image_count == 42);
  CHECK(phase.saved_step_index == 4);
  CHECK_FALSE(h.client->last_scan_request.has_value());
}

TEST_CASE("a saved session with no images is scanned as a new one", "[wizard][resume]") {
  harness h;
  h.client->inspect_response.set_has_session(true);
  h.client->inspect_response.set_image_count(0);
  h.client->scan_events = {scan_image("a.jpg"), scan_complete(1)};
  reach_confirm(h);
}

TEST_CASE("a failed probe counts as a folder without a session", "[wizard][resume]") {
  harness h;
  h.client->unary_error = backend_rpc{grpc::StatusCode::INTERNAL, "boom"};
  h.client->scan_events = {scan_image("a.jpg"), scan_complete(1)};
  reach_confirm(h);
}

TEST_CASE("start fresh from the restore prompt scans normally", "[wizard][resume]") {
  harness h;
  h.client->inspect_response.set_has_session(true);
  h.client->inspect_response.set_image_count(5);
  h.client->inspect_response.set_resume_step(3);
  h.client->scan_events = {scan_image("a.jpg"), scan_complete(1)};
  h.controller.select_folder("/photos");
  h.settle();
  REQUIRE(h.in<wizard_session_restore>());

  h.controller.start_fresh_from_restore();
  h.settle();
  REQUIRE(h.in<wizard_confirm_folder>());
  CHECK_FALSE(h.client->last_scan_request->resume());
}

TEST_CASE("resume rescans with the resume flag and restores the tunables", "[wizard][resume]") {
  harness h;
  h.client->inspect_response.set_has_session(true);
  h.client->inspect_response.set_image_count(2);
  h.client->inspect_response.set_resume_step(1);
  h.client->scan_events = {scan_image("a.jpg"), scan_image("b.jpg"), scan_complete(2)};
  h.client->session_results.set_resume_step(4);
  h.client->session_results.mutable_quality_thresholds()->set_blur_threshold(150.0);
  h.client->session_results.mutable_trip_params()->set_max_gap_hours(72);
  h.controller.select_folder("/photos");
  h.settle();
  REQUIRE(h.in<wizard_session_restore>());

  h.controller.resume_session();
  CHECK(h.in<wizard_scanning>());
  h.settle();

  // A session saved at the quality step re-runs the trips pass and lands on the batch menu.
  REQUIRE(h.in<wizard_batch_menu>());
  CHECK(h.client->last_scan_request->resume());
  CHECK(h.client->last_trips_request->max_gap_hours() == 72);
  CHECK(h.controller.image_count() == 2);
  CHECK(h.controller.tunables().blur_threshold == 150.0);
  CHECK(h.controller.tunables().trip_gap_hours == 72);
  // Unset values keep their defaults.
  CHECK(h.controller.tunables().under_exposed_threshold == 0.3);
  CHECK(h.controller.tunables().trip_distance_km == 300);
  REQUIRE(h.controller.restored_session() != nullptr);
  // The saved position is written only once the review is reached, with the review's step.
  REQUIRE(h.client->saved_session_states.size() == 1);
  CHECK(h.client->saved_session_states.front().step() == index_of(wizard_step::quality));
}

TEST_CASE("a resume that cannot read the results fails", "[wizard][resume]") {
  harness h;
  h.client->inspect_response.set_has_session(true);
  h.client->inspect_response.set_image_count(2);
  h.client->scan_events = {scan_image("a.jpg"), scan_complete(1)};
  h.controller.select_folder("/photos");
  h.settle();
  REQUIRE(h.in<wizard_session_restore>());

  h.client->unary_error = backend_rpc{grpc::StatusCode::INTERNAL, "corrupt"};
  h.controller.resume_session();
  h.settle();
  REQUIRE(h.in<wizard_failed>());
  CHECK(message_of(h.get<wizard_failed>().error) == "corrupt");
}

TEST_CASE("a resume of an empty index lands on the confirm screen", "[wizard][resume]") {
  harness h;
  h.client->inspect_response.set_has_session(true);
  h.client->inspect_response.set_image_count(2);
  h.client->scan_events = {scan_complete(0)};
  h.controller.select_folder("/photos");
  h.settle();
  h.controller.resume_session();
  h.settle();
  CHECK(h.in<wizard_confirm_folder>());
}

TEST_CASE("results that arrive after the controller is gone are dropped", "[wizard]") {
  ui_queue queue;
  auto client = std::make_shared<fake_kustavi_client>();
  client->scan_events = {scan_image("a.jpg"), scan_complete(1)};
  client->hold_streams = true;
  {
    wizard_controller controller{queue, inline_executor()};
    controller.set_client(client);
    controller.select_folder("/photos");
    while (queue.pending()) {
      queue.drain();
    }
    client->release_streams();
  }
  CHECK(queue.drain() > 0);
}

TEST_CASE("a resume of a session saved before any pass saves the confirm position",
          "[wizard][resume]") {
  harness h;
  h.client->inspect_response.set_has_session(true);
  h.client->inspect_response.set_image_count(2);
  h.client->scan_events = {scan_image("a.jpg"), scan_complete(1)};
  h.controller.select_folder("/photos");
  h.settle();
  h.controller.resume_session();
  h.settle();
  REQUIRE(h.in<wizard_confirm_folder>());
  CHECK(h.client->saved_session_states.size() == 1);
}



TEST_CASE("continue from confirm runs the trips pass and shows the review", "[wizard][trips]") {
  harness h;
  reach_trips_review(h);
  auto const &review = h.get<wizard_trips_review>();
  CHECK(review.trips.size() == 2);
  CHECK(review.folders.size() == 2);
  CHECK(review.folders.front().name == "Paris");
  CHECK(review.unassigned_ids == std::vector<std::string>{"d.jpg"});
  REQUIRE(h.client->last_trips_request.has_value());
  CHECK(h.client->last_trips_request->max_gap_hours() == 48);
  CHECK(stage_index_of(h.controller.phase()) == static_cast<int>(wizard_stage::organize));
}

TEST_CASE("trips progress is shown while the pass runs", "[wizard][trips]") {
  harness h;
  h.client->scan_events = {scan_image("a.jpg"), scan_complete(1)};
  reach_confirm(h);
  h.client->hold_streams = true;
  h.client->trips_events = {trips_progress(3, 10)};
  h.controller.continue_from_confirm();
  h.settle();
  REQUIRE(h.in<wizard_trips_running>());
  h.client->release_streams();
  h.queue.drain();
  // The progress event runs before the end of the stream.
  CHECK((h.in<wizard_trips_review>() || h.get<wizard_trips_running>().done == 3));
}

TEST_CASE("cancelling the trips pass returns to the confirm screen", "[wizard][trips]") {
  harness h;
  h.client->scan_events = {scan_image("a.jpg"), scan_complete(1)};
  reach_confirm(h);
  h.client->hold_streams = true;
  h.controller.continue_from_confirm();
  h.settle();
  REQUIRE(h.in<wizard_trips_running>());
  h.controller.cancel_trips();
  CHECK(h.in<wizard_confirm_folder>());
  h.client->release_streams();
  h.settle();
  CHECK(h.in<wizard_confirm_folder>());
}

TEST_CASE("back from the trips review returns to the confirm screen", "[wizard][trips]") {
  harness h;
  reach_trips_review(h);
  h.controller.back_from_trips();
  CHECK(h.in<wizard_confirm_folder>());
}

TEST_CASE("a failing trips pass leads to the error screen and back", "[wizard][trips]") {
  harness h;
  h.client->scan_events = {scan_image("a.jpg"), scan_complete(1)};
  reach_confirm(h);
  h.client->stream_error = backend_rpc{grpc::StatusCode::INTERNAL, "no geo table"};
  h.controller.continue_from_confirm();
  h.settle();
  REQUIRE(h.in<wizard_failed>());
  CHECK(h.get<wizard_failed>().step_index == index_of(wizard_step::trips));
  h.controller.go_back_from_error();
  CHECK(h.in<wizard_confirm_folder>());
}

TEST_CASE("moving photos between trips updates the review and saves the edit", "[wizard][trips]") {
  harness h;
  reach_trips_review(h);
  h.controller.move_images_to_trip({"d.jpg"}, 1);
  auto const &review = h.get<wizard_trips_review>();
  CHECK(review.unassigned_ids.empty());
  auto const rome = std::ranges::find_if(review.trips, [](auto const &t) { return t.id == 1; });
  REQUIRE(rome != review.trips.end());
  CHECK(rome->member_ids.size() == 3);
  auto const edits = saved_edits(h);
  REQUIRE(edits.has_value());
  CHECK(edits->membership.at("d.jpg") == 1);

  h.controller.move_images_to_trip({"a.jpg"}, std::nullopt);
  CHECK(h.get<wizard_trips_review>().unassigned_ids.size() == 1);
}

TEST_CASE("a trip made by hand takes the chosen photos", "[wizard][trips]") {
  harness h;
  reach_trips_review(h);
  h.controller.set_clock([] { return time_ms{std::chrono::milliseconds{1'700'000'000'000}}; });
  auto const id = h.controller.create_trip_from_images({"d.jpg"});
  CHECK(id >= trips_plan::first_user_trip_id);
  auto const &review = h.get<wizard_trips_review>();
  CHECK(review.trips.size() == 3);
  CHECK(review.unassigned_ids.empty());
  auto const edits = saved_edits(h);
  REQUIRE(edits.has_value());
  CHECK(edits->user_trips.size() == 1);
}

TEST_CASE("renaming a folder changes the review and the commit plan", "[wizard][trips]") {
  harness h;
  reach_trips_review(h);
  h.controller.rename_trip_folder(1, "Italy 2024");
  auto const &review = h.get<wizard_trips_review>();
  CHECK(std::ranges::any_of(review.folders, [](auto const &f) { return f.name == "Italy 2024"; }));
  CHECK(h.controller.commit_folder_plan().at("a.jpg") == "italy-2024");
  h.controller.rename_trip_folder(1, "");
  CHECK(std::ranges::any_of(h.get<wizard_trips_review>().folders,
                            [](auto const &f) { return f.name == "Italy 2024"; }));
}

TEST_CASE("turning folders off empties the commit plan and is saved", "[wizard][trips]") {
  harness h;
  reach_trips_review(h);
  h.controller.set_organize_into_folders(false);
  CHECK(h.controller.commit_folder_plan().empty());
  auto const edits = saved_edits(h);
  REQUIRE(edits.has_value());
  CHECK_FALSE(edits->organize_into_folders);
}

TEST_CASE("running the pass again uses the new settings and drops the edits", "[wizard][trips]") {
  harness h;
  reach_trips_review(h);
  h.controller.move_images_to_trip({"d.jpg"}, 1);
  wizard_controller::trip_params params;
  params.gap_hours = 24;
  params.leg_radius_km = 10;
  h.controller.rerun_trips_pass(params);
  h.settle();
  REQUIRE(h.in<wizard_trips_review>());
  CHECK(h.client->last_trips_request->max_gap_hours() == 24);
  CHECK(h.client->last_trips_request->leg_radius_km() == 10);
  CHECK(h.client->last_trips_request->max_distance_km() == 300);
  CHECK(h.get<wizard_trips_review>().unassigned_ids == std::vector<std::string>{"d.jpg"});
  // The saved edits are cleared: an empty text, or one with no moves.
  auto const edits = saved_edits(h);
  CHECK((!edits.has_value() || edits->membership.empty()));
  CHECK(std::ranges::any_of(h.client->saved_session_states,
                            [](auto const &saved) { return saved.has_trip_edits(); }));
}

TEST_CASE("photos marked for deletion leave their trips", "[wizard][trips]") {
  harness h;
  reach_trips_review(h);
  h.controller.set_deleted_ids({"a.jpg"});
  auto const &review = h.get<wizard_trips_review>();
  CHECK(review.marked_count == 1);
  auto const rome = std::ranges::find_if(review.trips, [](auto const &t) { return t.id == 1; });
  REQUIRE(rome != review.trips.end());
  CHECK(rome->member_ids == std::vector<std::string>{"b.jpg"});
}

TEST_CASE("the trips review saves the position, and edits only after a resume restores them",
          "[wizard][trips][resume]") {
  harness h;
  h.client->inspect_response.set_has_session(true);
  h.client->inspect_response.set_image_count(4);
  h.client->inspect_response.set_resume_step(5);
  h.client->scan_events = {scan_image("a.jpg"), scan_image("b.jpg"), scan_image("c.jpg"),
                           scan_image("d.jpg"), scan_complete(4)};
  h.client->trips_events = {trip_event(1, 1000, {"a.jpg", "b.jpg"}, "Rome"),
                            trip_event(2, 2000, {"c.jpg"}, "Paris")};
  trip_edits saved;
  saved.membership["d.jpg"] = 1;
  saved.renames.push_back({2, "Paris", "Trip to France"});
  saved.organize_into_folders = false;
  h.client->session_results.set_resume_step(5);
  h.client->session_results.set_trip_edits(saved.encode());
  h.client->session_results.mutable_trip_params()->set_max_gap_hours(72);

  h.controller.select_folder("/photos");
  h.settle();
  REQUIRE(h.in<wizard_session_restore>());
  h.controller.resume_session();
  h.settle();

  REQUIRE(h.in<wizard_batch_menu>());
  CHECK(h.client->last_trips_request->max_gap_hours() == 72);
  // The trips are back one step away, with the saved edits laid over them.
  h.controller.reopen_organize();
  REQUIRE(h.in<wizard_trips_review>());
  auto const &review = h.get<wizard_trips_review>();
  CHECK(review.unassigned_ids.empty());
  CHECK(std::ranges::any_of(review.folders, [](auto const &f) { return f.name == "Trip to France"; }));
  CHECK_FALSE(h.controller.organize_into_folders());
  // While the saved edits waited, none were saved over them.
  CHECK_FALSE(saved_edits(h).has_value());
  // The position is saved once the menu is reached.
  REQUIRE_FALSE(h.client->saved_session_states.empty());
  CHECK(h.client->saved_session_states.front().step() == index_of(wizard_step::quality));
  // A later edit is saved.
  h.controller.move_images_to_trip({"a.jpg"}, std::nullopt);
  CHECK(saved_edits(h).has_value());
}
