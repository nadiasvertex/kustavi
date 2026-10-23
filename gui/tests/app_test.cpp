#include <catch2/catch_test_macros.hpp>

#include "app/wizard_shell.h"
#include "testing/fake_kustavi_client.h"

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
    tree.set_viewport({1000, 700});
    controller.set_client(client);
    services.version = "1.2.3";
    services.exit_app = [this] { ++exits; };
    services.show_backend_log = [this] { ++log_requests; };
    services.pick_folder = [this] { ++picks; };
    shell = std::make_unique<wizard_shell>(tree, controller, model, loader, services);
    controller.on_change([this] { shell->mark_dirty(); });
    model.on_change([this](model_prep_state const &) { shell->mark_dirty(); });
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
  CHECK_FALSE(h.button("Continue")->enabled());

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
