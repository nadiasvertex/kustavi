#include <catch2/catch_test_macros.hpp>

#include "core/wizard_controller.h"
#include "testing/fake_kustavi_client.h"

#include <memory>
#include <string>
#include <vector>

using namespace kustavi::gui;
using kustavi::gui::testing::fake_kustavi_client;

namespace {

auto scan_image(std::string const &id) -> kustavi::ScanEvent {
  kustavi::ScanEvent event;
  auto *meta = event.mutable_image();
  meta->set_id(id);
  meta->set_name(id);
  meta->set_thumbnail_path("/cache/" + id);
  return event;
}

auto scan_complete(std::uint32_t images, std::vector<std::string> const &errors = {})
    -> kustavi::ScanEvent {
  kustavi::ScanEvent event;
  event.mutable_complete()->set_images(images);
  for (auto const &error : errors) {
    event.mutable_complete()->add_errors(error);
  }
  return event;
}

auto scan_progress(std::uint32_t seen, std::uint32_t found) -> kustavi::ScanEvent {
  kustavi::ScanEvent event;
  event.mutable_progress()->set_files_seen(seen);
  event.mutable_progress()->set_images_found(found);
  event.mutable_progress()->set_current_path("/photos/x");
  return event;
}

struct harness {
  harness() : controller(queue, inline_executor()) {
    controller.set_client(client);
    controller.on_change([this] { ++changes; });
  }

  // Runs what the workers posted, and what that posted, until the queue is quiet.
  auto settle() -> void {
    while (queue.pending()) {
      queue.drain();
    }
  }

  template <class Phase> [[nodiscard]] auto in() const -> bool {
    return std::holds_alternative<Phase>(controller.phase());
  }
  template <class Phase> [[nodiscard]] auto get() const -> Phase const & {
    return std::get<Phase>(controller.phase());
  }

  ui_queue queue;
  std::shared_ptr<fake_kustavi_client> client = std::make_shared<fake_kustavi_client>();
  wizard_controller controller;
  int changes = 0;
};

auto reach_confirm(harness &h, std::string const &folder = "/photos") -> void {
  h.controller.select_folder(folder);
  h.settle();
  REQUIRE(h.in<wizard_confirm_folder>());
}

} // namespace

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

  REQUIRE(h.in<wizard_confirm_folder>());
  CHECK(h.client->last_scan_request->resume());
  CHECK(h.get<wizard_confirm_folder>().image_count == 2);
  CHECK(h.controller.tunables().blur_threshold == 150.0);
  CHECK(h.controller.tunables().trip_gap_hours == 72);
  // Unset values keep their defaults.
  CHECK(h.controller.tunables().under_exposed_threshold == 0.3);
  CHECK(h.controller.tunables().trip_distance_km == 300);
  REQUIRE(h.controller.restored_session() != nullptr);
  // The saved step is not overwritten by the confirm screen.
  CHECK(h.client->saved_session_states.size() == 0);
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
