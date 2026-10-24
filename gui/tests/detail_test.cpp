#include <catch2/catch_test_macros.hpp>

#include "app/detail_view.h"
#include "controller_fixture.h"

#include <chrono>
#include <filesystem>
#include <thread>

using namespace kustavi::gui;
using namespace kustavi::gui::testing;

namespace {

// A player that makes the frames that a test gives it.
class fake_player final : public video_player {
public:
  explicit fake_player(std::function<void()> wake) : wake_(std::move(wake)) {}

  auto play() -> void override { playing_ = true; ++plays; }
  auto pause() -> void override { playing_ = false; ++pauses; }
  [[nodiscard]] auto playing() const -> bool override { return playing_; }
  [[nodiscard]] auto failed() const -> bool override { return failed_; }
  [[nodiscard]] auto take_frame() -> std::optional<video_frame> override {
    return std::exchange(next_, std::nullopt);
  }

  auto show(std::uint32_t width, std::uint32_t height) -> void {
    next_ = video_frame{width, height, std::vector<std::uint8_t>(std::size_t{width} * height * 4, 7)};
    wake_();
  }

  bool playing_ = false;
  bool failed_ = false;
  int plays = 0;
  int pauses = 0;

private:
  std::function<void()> wake_;
  std::optional<video_frame> next_;
};

auto photo_path() -> std::filesystem::path {
  for (auto const &entry : std::filesystem::directory_iterator{KUSTAVI_PHOTO_DIR}) {
    if (entry.path().extension() == ".jpg") {
      return entry.path();
    }
  }
  return {};
}

struct detail_harness {
  detail_harness() : controller(queue, inline_executor()), master(4096, 1, [] {}, true) {}

  auto make(detail_info info) -> void {
    view = std::make_unique<detail_view>(
        controller, std::move(info), master,
        [this](std::filesystem::path const &, std::function<void()> wake, int) {
          auto player = std::make_unique<fake_player>(std::move(wake));
          player_ = player.get();
          return std::unique_ptr<video_player>{std::move(player)};
        },
        [] {},
        detail_view::callbacks{[this] { ++closed; }, [this](image_info const &image) {
                                 opened = image.id;
                               }},
        basalt::size{1000, 700});
    root = view->build();
    tree.set_viewport({1000, 700});
    tree.set_root(std::move(root));
    tree.update_layout();
  }

  template <class T> auto find_all() -> std::vector<T *> {
    std::vector<T *> found;
    collect(*tree.root(), found);
    return found;
  }
  template <class T> static auto collect(basalt::widget &w, std::vector<T *> &found) -> void {
    if (auto *const match = dynamic_cast<T *>(&w)) {
      found.push_back(match);
    }
    for (std::size_t i = 0; i < w.child_count(); ++i) {
      collect(w.child(i), found);
    }
  }
  auto button(std::string const &label) -> basalt::button * {
    for (auto *const button : find_all<basalt::button>()) {
      if (button->label() == label) {
        return button;
      }
    }
    return nullptr;
  }
  auto wait_until(std::function<bool()> done) -> bool {
    for (int i = 0; i < 400 && !done(); ++i) {
      view->update();
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return done();
  }

  ui_queue queue;
  basalt::text::font_manager fonts;
  basalt::widget_tree tree{basalt::make_theme(basalt::color_scheme::light)};
  wizard_controller controller;
  image_loader master;
  std::unique_ptr<detail_view> view;
  std::unique_ptr<basalt::widget> root;
  fake_player *player_ = nullptr;
  int closed = 0;
  std::string opened;
};

auto sample_image() -> image_info {
  image_info image;
  image.id = "a.jpg";
  image.name = "a.jpg";
  image.path = photo_path().string();
  image.working_image_path = image.path;
  image.width = 4000;
  image.height = 3000;
  image.size_bytes = 3'500'000;
  return image;
}

} // namespace

TEST_CASE("the metadata rows list what is known about the photo", "[detail]") {
  detail_harness h;
  auto info = detail_info{};
  info.image = sample_image();
  info.image.taken = time_ms{std::chrono::milliseconds{1'775'000'000'000}};
  info.image.gps = geo_point{46.02, 7.74};
  info.sharpness = 12.34;
  info.exposure_score = 0.456;
  info.junk_reason = "screenshot";
  info.junk_confidence = 0.9;
  info.mappings = {{"Trip", "Alps · 2026-04-01"}, {"Place", ""}};
  h.make(std::move(info));
  auto const rows = h.view->metadata_rows();
  auto const has = [&](std::string const &label, std::string const &value) {
    return std::ranges::any_of(rows, [&](auto const &row) {
      return row.first == label && row.second == value;
    });
  };
  CHECK(has("Dimensions", "4000 × 3000"));
  CHECK(has("GPS", "46.02000, 7.74000"));
  CHECK(has("Sharpness", "12.3"));
  CHECK(has("Exposure", "0.46"));
  CHECK(has("Junk reason", "screenshot"));
  CHECK(has("Confidence", "90%"));
  CHECK(has("Trip", "Alps · 2026-04-01"));
  // A mapping with no value is left out.
  CHECK(std::ranges::none_of(rows, [](auto const &row) { return row.first == "Place"; }));
}

TEST_CASE("the preview shows first and the master replaces it", "[detail]") {
  REQUIRE_FALSE(photo_path().empty());
  detail_harness h;
  detail_info info;
  info.image = sample_image();
  h.make(std::move(info));
  auto *const zoom = h.find_all<zoom_view>().front();
  REQUIRE(h.wait_until([&] { return zoom->image() != nullptr; }));
  REQUIRE(h.wait_until([&] { return h.view->master_shown(); }));
  // The shape comes from the metadata, so the swap does not change the layout.
  CHECK(zoom->image() != nullptr);
}

TEST_CASE("the zoom buttons and the label follow the view", "[detail]") {
  detail_harness h;
  detail_info info;
  info.image = sample_image();
  h.make(std::move(info));
  auto *const zoom = h.find_all<zoom_view>().front();
  h.button("+")->press();
  CHECK(zoom->zoom() > 1);
  bool shown = false;
  for (auto *const label : h.find_all<basalt::label>()) {
    shown = shown || label->text() == "125%";
  }
  CHECK(shown);
  h.button("Reset")->press();
  CHECK(zoom->zoom() == 1);
  h.button("−")->press();
  CHECK(zoom->zoom() == 1);
}

TEST_CASE("the switch marks and keeps the photo", "[detail]") {
  detail_harness h;
  detail_info info;
  info.image = sample_image();
  info.can_toggle = true;
  h.make(std::move(info));
  auto *const toggle = h.find_all<basalt::toggle>().front();
  CHECK(toggle->enabled());
  CHECK_FALSE(toggle->on());
  toggle->activate();
  CHECK(h.controller.intent().explicit_deleted.contains("a.jpg"));
  toggle->activate();
  CHECK(h.controller.intent().explicit_kept.contains("a.jpg"));
  CHECK_FALSE(h.controller.intent().explicit_deleted.contains("a.jpg"));
}

TEST_CASE("the switch is off where the view only looks", "[detail]") {
  detail_harness h;
  detail_info info;
  info.image = sample_image();
  info.can_toggle = false;
  h.make(std::move(info));
  CHECK_FALSE(h.find_all<basalt::toggle>().front()->enabled());
}

TEST_CASE("close and the nearby shots call back", "[detail]") {
  detail_harness h;
  detail_info info;
  info.image = sample_image();
  image_info neighbor;
  neighbor.id = "b.jpg";
  neighbor.name = "b.jpg";
  info.neighbors = {neighbor};
  h.make(std::move(info));
  h.button("b.jpg")->press();
  CHECK(h.opened == "b.jpg");
  h.button("Close")->press();
  CHECK(h.closed == 1);
}

TEST_CASE("a video plays in place of the picture", "[detail][video]") {
  detail_harness h;
  detail_info info;
  info.image = sample_image();
  info.image.is_video = true;
  info.image.width = 1920;
  info.image.height = 1080;
  h.make(std::move(info));
  REQUIRE(h.player_ != nullptr);
  auto *const play = h.button("Play");
  REQUIRE(play != nullptr);
  CHECK_FALSE(play->visible());

  h.player_->show(64, 36);
  h.view->update();
  auto *const zoom = h.find_all<zoom_view>().front();
  REQUIRE(zoom->image() != nullptr);
  CHECK(zoom->image()->width() == 64);
  // The first frame starts the playback and shows the button.
  CHECK(play->visible());
  CHECK(h.player_->plays == 1);
  CHECK(play->label() == "Pause");

  play->press();
  CHECK(h.player_->pauses == 1);
  CHECK(play->label() == "Play");
  play->press();
  CHECK(h.player_->plays == 2);

  // A later frame replaces the picture without touching the playback.
  h.player_->show(128, 72);
  h.view->update();
  CHECK(zoom->image()->width() == 128);
  CHECK(h.player_->plays == 2);
}

TEST_CASE("a player that fails leaves the still image", "[detail][video]") {
  detail_harness h;
  detail_info info;
  info.image = sample_image();
  info.image.is_video = true;
  h.make(std::move(info));
  REQUIRE(h.player_ != nullptr);
  h.player_->failed_ = true;
  h.view->update();
  CHECK(h.view->video() == nullptr);
  CHECK_FALSE(h.button("Play")->visible());
}

TEST_CASE("a video with no player in the build shows the still image", "[detail][video]") {
  detail_harness h;
  detail_info info;
  info.image = sample_image();
  info.image.is_video = true;
  info.image.working_image_path = sample_image().path;
  h.view = std::make_unique<detail_view>(
      h.controller, std::move(info), h.master, video_player_factory{}, [] {},
      detail_view::callbacks{}, basalt::size{1000, 700});
  h.tree.set_viewport({1000, 700});
  h.tree.set_root(h.view->build());
  h.tree.update_layout();
  CHECK(h.view->video() == nullptr);
  auto *const zoom = h.find_all<zoom_view>().front();
  REQUIRE(h.wait_until([&] { return zoom->image() != nullptr; }));
}
