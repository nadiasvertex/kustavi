#include <catch2/catch_test_macros.hpp>

#include "app/batch_menu_screen.h"
#include "app/trips_screen.h"
#include "app/wizard_shell.h"
#include "testing/fake_kustavi_client.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace kustavi::gui;
using kustavi::gui::testing::fake_kustavi_client;

namespace {

auto scan_image(std::string const &id) -> kustavi::ScanEvent {
  kustavi::ScanEvent event;
  event.mutable_image()->set_id(id);
  event.mutable_image()->set_name(id);
  return event;
}

auto scan_complete(std::uint32_t images) -> kustavi::ScanEvent {
  kustavi::ScanEvent event;
  event.mutable_complete()->set_images(images);
  return event;
}

// A shell over a controller and a fake back end, with no window. The tree is laid out at a fixed
// size, so the widgets have bounds and a click could reach them.
struct harness {
  harness()
      : tree(basalt::make_theme(basalt::color_scheme::light)), controller(queue, inline_executor()),
        model(queue), loader(64, 1, [] {}) {
    fonts.add_system_fonts();
    shaper = std::make_unique<basalt::text::text_shaper>(fonts);
    tree.set_text_shaper(shaper.get());
    tree.set_viewport({1000, 1400});
    controller.set_client(client);
    services.version = "1.2.3";
    services.exit_app = [this] { ++exits; };
    services.show_backend_log = [this] { ++log_requests; };
    services.pick_folder = [this] { ++picks; };
    shell = std::make_unique<wizard_shell>(tree, controller, model, loader, services);
    controller.on_change([this] { shell->phase_changed(); });
    model.on_change([this](model_prep_state const &) { shell->mark_dirty(); });
    router = std::make_unique<basalt::event_router>(tree);
    refresh();
  }

  auto click(basalt::widget &target) -> void {
    auto const at = target.bounds_in_root().center();
    router->dispatch(basalt::pointer_event{.action = basalt::pointer_action::down,
                                           .position = at,
                                           .button = basalt::pointer_button::primary,
                                           .buttons = basalt::pointer_button::primary});
    router->dispatch(basalt::pointer_event{.action = basalt::pointer_action::up,
                                           .position = at,
                                           .button = basalt::pointer_button::primary});
    refresh();
  }

  auto press_key(basalt::key code) -> void {
    router->dispatch(basalt::key_event{.code = code, .pressed = true});
    refresh();
  }

  ~harness() {
    // The shell refers to the tree and the controller.
    shell.reset();
  }

  auto refresh() -> void {
    while (queue.pending()) {
      queue.drain();
    }
    shell->update();
    tree.update_layout();
  }

  template <class T> static auto collect(basalt::widget &w, std::vector<T *> &found) -> void {
    if (auto *const match = dynamic_cast<T *>(&w)) {
      found.push_back(match);
    }
    for (std::size_t i = 0; i < w.child_count(); ++i) {
      collect(w.child(i), found);
    }
  }

  template <class T> auto find_all() -> std::vector<T *> {
    std::vector<T *> found;
    collect(*tree.root(), found);
    return found;
  }

  auto button(std::string const &label) -> basalt::button * {
    for (auto *const button : find_all<basalt::button>()) {
      if (button->label() == label) {
        return button;
      }
    }
    return nullptr;
  }

  auto has_text(std::string const &text) -> bool {
    for (auto *const label : find_all<basalt::label>()) {
      if (label->text() == text) {
        return true;
      }
    }
    return false;
  }

  auto has_text_containing(std::string const &part) -> bool {
    for (auto *const label : find_all<basalt::label>()) {
      if (label->text().find(part) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

  ui_queue queue;
  basalt::text::font_manager fonts;
  std::unique_ptr<basalt::text::text_shaper> shaper;
  basalt::widget_tree tree;
  std::shared_ptr<fake_kustavi_client> client = std::make_shared<fake_kustavi_client>();
  wizard_controller controller;
  model_status model;
  image_loader loader;
  shell_services services;
  std::unique_ptr<wizard_shell> shell;
  std::unique_ptr<basalt::event_router> router;
  int exits = 0;
  int log_requests = 0;
  int picks = 0;
};

} // namespace

TEST_CASE("the start screen offers a folder and shows the version", "[app]") {
  harness h;
  CHECK(h.has_text("Kustavi"));
  CHECK(h.has_text("Kustavi v1.2.3"));
  auto *const select = h.button("Select folder…");
  REQUIRE(select != nullptr);
  CHECK(select->press());
  CHECK(h.picks == 1);
}

TEST_CASE("the start screen has no action bar", "[app]") {
  harness h;
  CHECK(h.find_all<basalt::button>().size() == 1);
}

TEST_CASE("the model card shows only while a download runs", "[app]") {
  harness h;
  CHECK_FALSE(h.has_text_containing("Preparing vision model"));
  CHECK(h.find_all<basalt::progress_bar>().empty());

  kustavi::ModelEvent progress;
  progress.mutable_progress()->set_done_bytes(1'200'000'000);
  progress.mutable_progress()->set_total_bytes(2'900'000'000);
  h.client->model_events = {progress};
  h.client->hold_streams = true;
  h.model.start(h.client);
  h.client->release_streams();
  h.refresh();
  CHECK(h.has_text_containing("Preparing vision model — 41%"));
  REQUIRE(h.find_all<basalt::progress_bar>().size() == 1);
  CHECK(h.find_all<basalt::progress_bar>().front()->fraction().has_value());
}

TEST_CASE("the model card caption without a total", "[app]") {
  CHECK(model_card_caption(model_prep_downloading{100, 0, 0}) == "Preparing vision model…");
  CHECK(model_card_caption(model_prep_downloading{600, 1200, 0}).starts_with(
      "Preparing vision model — 50%"));
}

TEST_CASE("the step indicator follows the stage of the phase", "[app]") {
  harness h;
  auto labels = h.find_all<basalt::label>();
  auto const tone_of = [&](std::string const &text) {
    for (auto *const label : h.find_all<basalt::label>()) {
      if (label->text() == text) {
        return label->tone();
      }
    }
    return basalt::text_tone::normal;
  };
  CHECK(tone_of("Select") == basalt::text_tone::interactive);
  CHECK(tone_of("Organize") == basalt::text_tone::subdued);
  CHECK(tone_of("Copy") == basalt::text_tone::subdued);
}

TEST_CASE("a scan shows its progress and can be cancelled", "[app]") {
  harness h;
  h.client->hold_streams = true;
  h.client->scan_events = {scan_image("a.jpg"), scan_complete(1)};
  h.controller.select_folder("/photos");
  h.refresh();
  REQUIRE(std::holds_alternative<wizard_scanning>(h.controller.phase()));
  CHECK(h.has_text("Scanning /photos"));
  CHECK(h.has_text("Found 0 images"));
  CHECK(h.shell->animating());

  h.client->release_streams();
  // The events and the end of the stream arrive together here, so the scan ends at once.
  h.refresh();
  CHECK(std::holds_alternative<wizard_confirm_folder>(h.controller.phase()));
}

TEST_CASE("the cancel button returns to the start", "[app]") {
  harness h;
  h.client->hold_streams = true;
  h.controller.select_folder("/photos");
  h.refresh();
  auto *const cancel = h.button("Cancel");
  REQUIRE(cancel != nullptr);
  cancel->press();
  h.refresh();
  CHECK(std::holds_alternative<wizard_start>(h.controller.phase()));
  CHECK(h.button("Select folder…") != nullptr);
  CHECK_FALSE(h.shell->animating());
}

TEST_CASE("the confirm screen counts the images and offers back and continue", "[app]") {
  harness h;
  h.client->scan_events = {scan_image("a.jpg"), scan_image("b.jpg"), scan_complete(2)};
  h.controller.select_folder("/photos");
  h.refresh();
  REQUIRE(std::holds_alternative<wizard_confirm_folder>(h.controller.phase()));
  CHECK(h.has_text("2 images in /photos"));
  CHECK(h.find_all<basalt::grid_view>().size() == 1);
  REQUIRE(h.button("Continue") != nullptr);
  CHECK(h.button("Continue")->enabled());

  h.button("Back")->press();
  h.refresh();
  CHECK(std::holds_alternative<wizard_start>(h.controller.phase()));
  CHECK(h.find_all<basalt::grid_view>().empty());
}

TEST_CASE("unreadable files are counted on the confirm screen", "[app]") {
  harness h;
  auto complete = scan_complete(1);
  complete.mutable_complete()->add_errors("x.jpg: truncated");
  complete.mutable_complete()->add_errors("y.jpg: truncated");
  h.client->scan_events = {scan_image("a.jpg"), complete};
  h.controller.select_folder("/photos");
  h.refresh();
  CHECK(h.has_text("2 files could not be read"));
}

TEST_CASE("a folder without images offers another folder or leaving", "[app]") {
  harness h;
  h.client->scan_events = {scan_complete(0)};
  h.controller.select_folder("/empty");
  h.refresh();
  CHECK(h.has_text("No images found in /empty"));
  h.button("Exit app")->press();
  CHECK(h.exits == 1);
  h.button("Choose another folder")->press();
  h.refresh();
  CHECK(std::holds_alternative<wizard_start>(h.controller.phase()));
}

TEST_CASE("the restore prompt names the saved step", "[app]") {
  harness h;
  h.client->inspect_response.set_has_session(true);
  h.client->inspect_response.set_image_count(42);
  h.client->inspect_response.set_resume_step(3);
  h.controller.select_folder("/photos");
  h.refresh();
  CHECK(h.has_text("Resume this session?"));
  CHECK(h.has_text_containing("42 images, stopped at the \"Junk\" step"));
  CHECK(h.find_all<basalt::button>().size() == 2);
}

TEST_CASE("the restore prompt buttons resume or start fresh", "[app]") {
  harness h;
  h.client->inspect_response.set_has_session(true);
  h.client->inspect_response.set_image_count(2);
  h.client->scan_events = {scan_image("a.jpg"), scan_complete(1)};
  h.controller.select_folder("/photos");
  h.refresh();
  h.button("Start fresh")->press();
  h.refresh();
  CHECK(std::holds_alternative<wizard_confirm_folder>(h.controller.phase()));
  CHECK_FALSE(h.client->last_scan_request->resume());
}

TEST_CASE("the error screen shows the message and goes back", "[app]") {
  harness h;
  h.client->stream_error = backend_rpc{grpc::StatusCode::INTERNAL, "disk failure"};
  h.controller.select_folder("/photos");
  h.refresh();
  CHECK(h.has_text("Processing error"));
  CHECK(h.has_text("disk failure"));
  h.button("Show back-end log")->press();
  CHECK(h.log_requests == 1);
  h.button("Back")->press();
  h.refresh();
  CHECK(std::holds_alternative<wizard_start>(h.controller.phase()));
}

TEST_CASE("the crash dialog retries or exits", "[app]") {
  harness h;
  int retries = 0;
  h.shell->show_crash_dialog("The back end stopped unexpectedly.", [&] { ++retries; });
  h.refresh();
  CHECK(h.shell->overlays().overlay_count() == 1);
  auto dialogs = h.find_all<basalt::dialog>();
  REQUIRE(dialogs.size() == 1);
  CHECK(dialogs.front()->choose(1));
  CHECK(retries == 1);
  CHECK(h.shell->overlays().overlay_count() == 0);

  h.shell->show_crash_dialog("again", [&] { ++retries; });
  h.refresh();
  REQUIRE(h.find_all<basalt::dialog>().size() == 1);
  h.find_all<basalt::dialog>().front()->choose(0);
  CHECK(h.exits == 1);
  CHECK(retries == 1);
}

TEST_CASE("the log dialog shows the last lines and closes", "[app]") {
  harness h;
  std::string log;
  for (int i = 0; i < 100; ++i) {
    log += "line " + std::to_string(i) + "\n";
  }
  h.shell->show_log_dialog(log);
  h.refresh();
  REQUIRE(h.shell->overlays().overlay_count() == 1);
  h.find_all<basalt::dialog>().front()->choose(0);
  CHECK(h.shell->overlays().overlay_count() == 0);
}

namespace {

auto trip_event(std::uint32_t id, std::int64_t start_ms, std::vector<std::string> const &ids,
                std::string const &folder) -> kustavi::TripsEvent {
  kustavi::TripsEvent event;
  auto *trip = event.mutable_trip();
  trip->set_id(id);
  trip->set_start_unix_ms(start_ms);
  trip->set_end_unix_ms(start_ms + 1000);
  for (auto const &image : ids) {
    trip->add_image_ids(image);
  }
  trip->set_folder(folder);
  trip->set_folder_slug(folder);
  return event;
}

// Four photos: a and b in "Rome", c in "Paris", and d in no trip. Ends on the trips review.
auto reach_trips(harness &h) -> void {
  h.client->scan_events = {scan_image("a.jpg"), scan_image("b.jpg"), scan_image("c.jpg"),
                           scan_image("d.jpg"), scan_complete(4)};
  h.client->trips_events = {trip_event(1, 1000, {"a.jpg", "b.jpg"}, "Rome"),
                            trip_event(2, 2000, {"c.jpg"}, "Paris")};
  h.controller.select_folder("/photos");
  h.refresh();
  h.button("Continue")->press();
  h.refresh();
  REQUIRE(std::holds_alternative<wizard_trips_review>(h.controller.phase()));
}

} // namespace

TEST_CASE("continue on the confirm screen leads to the trips review", "[app][trips]") {
  harness h;
  reach_trips(h);
  CHECK(h.has_text("2 folders · 2 trips · 0 marked · 1 unassigned"));
  CHECK(h.has_text("Paris"));
  CHECK(h.has_text("Rome"));
  CHECK(h.has_text("Unassigned · 1 photos"));
  CHECK(h.find_all<basalt::grid_view>().size() == 1);
  CHECK(h.button("Back") != nullptr);
  CHECK(h.button("Continue")->enabled());
}

TEST_CASE("the trips pass shows a progress screen that can be cancelled", "[app][trips]") {
  harness h;
  h.client->scan_events = {scan_image("a.jpg"), scan_complete(1)};
  h.controller.select_folder("/photos");
  h.refresh();
  h.client->hold_streams = true;
  h.button("Continue")->press();
  h.refresh();
  CHECK(h.has_text("Grouping by trip"));
  h.button("Cancel")->press();
  h.refresh();
  CHECK(std::holds_alternative<wizard_confirm_folder>(h.controller.phase()));
}

TEST_CASE("a clustering with no trips and no loose photos says so", "[app][trips]") {
  harness h;
  h.client->scan_events = {scan_image("a.jpg"), scan_complete(1)};
  h.client->trips_events = {trip_event(1, 1000, {"a.jpg"}, "Rome")};
  h.controller.select_folder("/photos");
  h.refresh();
  h.button("Continue")->press();
  h.refresh();
  h.controller.set_deleted_ids({"a.jpg"});
  h.refresh();
  CHECK(h.has_text("No trips found — photos may lack GPS or timestamp data"));
}

TEST_CASE("the settings open on demand and re-cluster with the slider values", "[app][trips]") {
  harness h;
  reach_trips(h);
  CHECK(h.find_all<basalt::slider>().empty());
  h.button("Clustering settings")->press();
  h.refresh();
  auto sliders = h.find_all<basalt::slider>();
  REQUIRE(sliders.size() == 4);
  CHECK(sliders.front()->value() == 48);
  CHECK(sliders.front()->label() == "Max time gap");

  h.tree.focus().set_focus(*sliders.front());
  h.press_key(basalt::key::right);
  CHECK(sliders.front()->value() == 49);
  h.button("Re-cluster")->press();
  h.refresh();
  CHECK(h.client->last_trips_request->max_gap_hours() == 49);
  CHECK(h.controller.tunables().trip_gap_hours == 49);
  REQUIRE(std::holds_alternative<wizard_trips_review>(h.controller.phase()));
}

TEST_CASE("the folders toggle is saved with the edits", "[app][trips]") {
  harness h;
  reach_trips(h);
  h.button("Clustering settings")->press();
  h.refresh();
  auto toggles = h.find_all<basalt::toggle>();
  REQUIRE(toggles.size() == 1);
  CHECK(toggles.front()->on());
  h.click(*toggles.front());
  CHECK_FALSE(h.controller.organize_into_folders());
}

TEST_CASE("selected photos move to a trip from the menu", "[app][trips]") {
  harness h;
  reach_trips(h);
  CHECK(h.button("Move to trip ▸") == nullptr);
  h.button("Select photos")->press();
  h.refresh();
  CHECK(h.button("Cancel selection") != nullptr);

  auto grids = h.find_all<basalt::grid_view>();
  REQUIRE(grids.size() == 1);
  // The photos are in the order of the sections: Paris (c), Rome (a, b), then the unassigned d.
  auto const last = grids.front()->item_count() - 1;
  REQUIRE(grids.front()->cell(last) != nullptr);
  h.click(*grids.front()->cell(last));
  CHECK(h.has_text("1 selected"));
  auto *const move = h.button("Move to trip ▸");
  REQUIRE(move != nullptr);
  h.click(*move);

  auto items = h.find_all<basalt::menu_item>();
  REQUIRE_FALSE(items.empty());
  CHECK(items.front()->label() == "New trip");
  auto const rome = std::ranges::find_if(items, [](auto *item) {
    return item->item_id() == "trip:1";
  });
  REQUIRE(rome != items.end());
  CHECK((*rome)->activate());
  h.refresh();

  auto const &review = std::get<wizard_trips_review>(h.controller.phase());
  CHECK(review.unassigned_ids.empty());
  CHECK(h.button("Select photos") != nullptr);
  CHECK_FALSE(h.has_text_containing("selected"));
}

TEST_CASE("cancelling the selection clears it", "[app][trips]") {
  harness h;
  reach_trips(h);
  h.button("Select photos")->press();
  h.refresh();
  auto grids = h.find_all<basalt::grid_view>();
  h.click(*grids.front()->cell(0));
  CHECK(h.has_text("1 selected"));
  h.button("Cancel selection")->press();
  h.refresh();
  CHECK_FALSE(h.has_text_containing("selected"));
  CHECK(h.button("Select photos") != nullptr);
}

TEST_CASE("a folder is renamed in place", "[app][trips]") {
  harness h;
  reach_trips(h);
  REQUIRE(h.button("Rename") != nullptr);
  h.button("Rename")->press();
  h.refresh();
  auto fields = h.find_all<basalt::text_field>();
  REQUIRE(fields.size() == 1);
  CHECK(fields.front()->text() == "Paris");
  fields.front()->set_text("France");
  h.tree.focus().set_focus(*fields.front());
  h.press_key(basalt::key::enter);
  h.refresh();
  auto const &review = std::get<wizard_trips_review>(h.controller.phase());
  CHECK(std::ranges::any_of(review.folders, [](auto const &f) { return f.name == "France"; }));
  CHECK(h.find_all<basalt::text_field>().empty());
}

TEST_CASE("the summary counts folders, trips, marks and loose photos", "[app][trips]") {
  wizard_trips_review review;
  CHECK(trips_screen::summary_text(review) == "0 folders · 0 trips · 0 marked");
  review.trips.resize(1);
  review.folders.resize(1);
  review.marked_count = 3;
  review.unassigned_ids = {"a", "b"};
  CHECK(trips_screen::summary_text(review) == "1 folder · 1 trip · 3 marked · 2 unassigned");
}

namespace {

auto quality_flag(std::string const &id) -> kustavi::QualityEvent {
  kustavi::QualityEvent event;
  event.mutable_flag()->set_image_id(id);
  event.mutable_flag()->add_reasons(kustavi::BLURRY);
  return event;
}

auto reach_menu_screen(harness &h) -> void {
  reach_trips(h);
  h.button("Continue")->press();
  h.refresh();
  REQUIRE(std::holds_alternative<wizard_batch_menu>(h.controller.phase()));
}

// The text of the labels in the cells of every grid.
auto cell_captions(harness &h) -> std::vector<std::string> {
  std::vector<std::string> captions;
  for (auto *const tile : h.find_all<basalt::image_tile>()) {
    captions.push_back(tile->caption());
  }
  std::ranges::sort(captions);
  return captions;
}

} // namespace

TEST_CASE("the batch menu lists the batches and the passes of the selected one", "[app][batches]") {
  harness h;
  reach_menu_screen(h);
  CHECK(h.has_text("3 batches · 0 of 4 photos marked for deletion"));
  CHECK(h.button("Paris  ·  1 photos") != nullptr);
  CHECK(h.has_text("Quality"));
  CHECK(h.has_text("Duplicates"));
  CHECK(h.has_text("Junk"));
  // None of the photos is a video.
  CHECK_FALSE(h.has_text("Video"));
  CHECK(h.has_text("Not run"));
  CHECK(h.button("Edit trips") != nullptr);
  CHECK_FALSE(h.button("Continue")->enabled());

  h.button("Rome  ·  2 photos")->press();
  h.refresh();
  CHECK(h.has_text("Rome"));
  CHECK(std::get<wizard_batch_menu>(h.controller.phase()).selected_key == "Rome");
}

TEST_CASE("the pass summaries", "[app][batches]") {
  CHECK(pass_summary(wizard_step::quality, {}) == "Not run");
  CHECK(pass_summary(wizard_step::quality, {false, 2}) == "Partly run · 2 flagged");
  CHECK(pass_summary(wizard_step::quality, {true, 3}) == "Done · 3 flagged");
  CHECK(pass_summary(wizard_step::duplicates, {true, 3}) == "Done · 3 groups");
}

TEST_CASE("running a pass shows its progress and then its review", "[app][batches]") {
  harness h;
  reach_menu_screen(h);
  h.client->quality_events = {quality_flag("c.jpg")};
  h.client->hold_streams = true;
  h.find_all<basalt::button>();
  // Paris is the first batch, so "Run" on the first card is the quality pass.
  auto runs = std::vector<basalt::button *>{};
  for (auto *const button : h.find_all<basalt::button>()) {
    if (button->label() == "Run") {
      runs.push_back(button);
    }
  }
  REQUIRE(runs.size() == 3);
  runs.front()->press();
  h.refresh();
  CHECK(h.has_text("Checking image quality"));
  CHECK(h.button("Cancel") != nullptr);

  h.client->release_streams();
  h.refresh();
  REQUIRE(std::holds_alternative<wizard_quality_review>(h.controller.phase()));
  CHECK(h.has_text("1 of 1 images flagged"));
  CHECK(h.has_text("Keeping 0"));
  CHECK(h.has_text("Click a photo below to keep it"));
  CHECK(h.button("▾ Deleting 1") != nullptr);
  CHECK(h.button("Keep all") != nullptr);
  CHECK(h.button("Mark all") != nullptr);
}

TEST_CASE("a click moves a photo between the keep and delete sections", "[app][batches]") {
  harness h;
  reach_menu_screen(h);
  h.client->quality_events = {quality_flag("c.jpg")};
  h.controller.start_batch_pass(wizard_step::quality);
  h.refresh();
  auto grids = h.find_all<basalt::grid_view>();
  REQUIRE(grids.size() == 1);
  h.click(*grids.front()->cell(0));
  CHECK(h.has_text("Keeping 1"));
  CHECK(h.controller.intent().explicit_kept.contains("c.jpg"));
  CHECK(h.button("▾ Deleting 0") != nullptr);

  grids = h.find_all<basalt::grid_view>();
  REQUIRE(grids.size() == 1);
  h.click(*grids.front()->cell(0));
  CHECK(h.has_text("Keeping 0"));
  CHECK(h.controller.is_marked("c.jpg", deletion_step::quality));
}

TEST_CASE("keep all, mark all and done on a review", "[app][batches]") {
  harness h;
  reach_menu_screen(h);
  h.client->quality_events = {quality_flag("c.jpg")};
  h.controller.start_batch_pass(wizard_step::quality);
  h.refresh();
  h.button("Keep all")->press();
  h.refresh();
  CHECK(h.has_text("Keeping 1"));
  h.button("Mark all")->press();
  h.refresh();
  CHECK(h.has_text("Keeping 0"));
  h.button("Done")->press();
  h.refresh();
  CHECK(std::holds_alternative<wizard_batch_menu>(h.controller.phase()));
  CHECK(h.has_text_containing("1 of 4 photos marked for deletion"));
}

TEST_CASE("the review of a pass with no flags says so", "[app][batches]") {
  harness h;
  reach_menu_screen(h);
  h.controller.start_batch_pass(wizard_step::quality);
  h.refresh();
  CHECK(h.has_text("No blurry or poorly exposed images found"));
  CHECK(h.button("Keep all") == nullptr);
}

TEST_CASE("the thresholds open without the sliders being rebuilt by a change",
          "[app][batches]") {
  harness h;
  reach_menu_screen(h);
  h.client->quality_events = {quality_flag("c.jpg")};
  h.controller.start_batch_pass(wizard_step::quality);
  h.refresh();
  CHECK(h.find_all<basalt::slider>().empty());
  CHECK(h.has_text("Blur 100 · Under 0.30 · Over 0.30"));
  h.button("▸ Quality thresholds")->press();
  h.refresh();
  auto sliders = h.find_all<basalt::slider>();
  REQUIRE(sliders.size() == 3);
  CHECK(sliders.front()->label() == "Blur threshold");
  CHECK(h.has_text("1 flagged at the last run"));
  CHECK(h.button("Rerun pass") != nullptr);
  CHECK_FALSE(h.button("Rerun pass")->visible());

  auto *const blur = sliders.front();
  h.client->preview_response.set_flagged(5);
  h.tree.focus().set_focus(*blur);
  h.press_key(basalt::key::right);
  // The same slider is still in the tree, and the panel shows the answer.
  auto const after = h.find_all<basalt::slider>();
  REQUIRE(after.size() == 3);
  CHECK(after.front() == blur);
  CHECK(blur->value() == 110);
  CHECK(h.controller.tunables().blur_threshold == 110);
  CHECK(h.has_text("These settings flag 5 of 1 images (1 at the last run)"));
  CHECK(h.button("Rerun pass")->visible());

  h.button("Reset to defaults")->press();
  h.refresh();
  CHECK(h.controller.tunables().blur_threshold == 100);
}

TEST_CASE("a duplicate review marks the keeper and a click chooses another", "[app][batches]") {
  harness h;
  reach_menu_screen(h);
  h.controller.select_batch("Rome");
  kustavi::SimilarEvent group;
  group.mutable_group()->set_id(0);
  group.mutable_group()->add_image_ids("a.jpg");
  group.mutable_group()->add_image_ids("b.jpg");
  group.mutable_group()->set_recommended_keep_id("a.jpg");
  h.client->similar_events = {group};
  h.controller.start_batch_pass(wizard_step::duplicates);
  h.refresh();
  CHECK(h.has_text("1 groups · 2 images"));
  auto grids = h.find_all<basalt::grid_view>();
  REQUIRE(grids.size() == 1);
  CHECK(h.has_text("Group 1 · 2 photos"));
  // The first cell is the suggested keeper and the second is marked.
  auto *const first = dynamic_cast<basalt::image_tile *>(grids.front()->cell(0));
  auto *const second = dynamic_cast<basalt::image_tile *>(grids.front()->cell(1));
  REQUIRE(first != nullptr);
  REQUIRE(second != nullptr);
  CHECK(first->selected());
  CHECK_FALSE(second->selected());
  h.click(*second);
  CHECK(h.controller.intent().user_keeper_of(0) == "b.jpg");
  CHECK(h.controller.is_marked("a.jpg", deletion_step::similar));
  h.button("Done")->press();
  h.refresh();
  CHECK(std::holds_alternative<wizard_batch_menu>(h.controller.phase()));
}

TEST_CASE("the junk pass waits on a download screen", "[app][batches]") {
  harness h;
  reach_menu_screen(h);
  auto runs = std::vector<basalt::button *>{};
  for (auto *const button : h.find_all<basalt::button>()) {
    if (button->label() == "Run") {
      runs.push_back(button);
    }
  }
  REQUIRE(runs.size() == 3);
  runs.back()->press();
  h.refresh();
  REQUIRE(std::holds_alternative<wizard_junk_prep>(h.controller.phase()));
  CHECK(h.has_text("Downloading vision model"));
  CHECK(h.button("Cancel") != nullptr);

  kustavi::ModelEvent progress;
  progress.mutable_progress()->set_done_bytes(1'200'000'000);
  progress.mutable_progress()->set_total_bytes(2'900'000'000);
  progress.mutable_progress()->set_speed_bps(12e6);
  h.client->model_events = {progress};
  h.model.start(h.client);
  h.refresh();
  CHECK(h.has_text_containing("12.0 MB/s"));

  h.button("Cancel")->press();
  h.refresh();
  CHECK(std::holds_alternative<wizard_batch_menu>(h.controller.phase()));
}

TEST_CASE("a failed model download offers a retry", "[app][batches]") {
  harness h;
  reach_menu_screen(h);
  h.services.retry_download = [&h] { h.model.retry_download(); };
  h.client->stream_error = backend_rpc{grpc::StatusCode::UNAVAILABLE, "offline"};
  h.model.start(h.client);
  h.controller.start_batch_pass(wizard_step::junk);
  h.refresh();
  CHECK(h.has_text("The vision model download failed"));
  CHECK(h.has_text("offline"));
  CHECK(h.button("Retry download") != nullptr);
}

TEST_CASE("the edit trips button returns to the trips review", "[app][batches]") {
  harness h;
  reach_menu_screen(h);
  h.button("Edit trips")->press();
  h.refresh();
  CHECK(std::holds_alternative<wizard_trips_review>(h.controller.phase()));
}
