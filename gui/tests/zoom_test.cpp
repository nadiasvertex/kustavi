#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "app/zoom_view.h"

using namespace kustavi::gui;
using Catch::Approx;

namespace {

constexpr basalt::size view{800, 600};
constexpr basalt::size wide{4000, 2000};

} // namespace

TEST_CASE("an image is fitted into the view with its shape kept", "[zoom]") {
  auto const fitted = zoom_state::fitted(view, wide);
  CHECK(fitted.width == Approx(800));
  CHECK(fitted.height == Approx(400));
  auto const tall = zoom_state::fitted(view, {1000, 4000});
  CHECK(tall.height == Approx(600));
  CHECK(tall.width == Approx(150));
  CHECK(zoom_state::fitted(view, {0, 0}).width == 0);
  CHECK(zoom_state::fitted({0, 0}, wide).width == 0);
}

TEST_CASE("at zoom one the image is centered", "[zoom]") {
  zoom_state state;
  auto const rect = state.image_rect(view, wide);
  CHECK(rect.origin.x == Approx(0));
  CHECK(rect.origin.y == Approx(100));
  CHECK(rect.extent.width == Approx(800));
}

TEST_CASE("zooming keeps the point under the anchor still", "[zoom]") {
  zoom_state state;
  basalt::point const anchor{600, 300};
  auto const before = state.image_rect(view, wide);
  // The anchor's place in the image, from 0 to 1.
  auto const fx = (anchor.x - before.origin.x) / before.extent.width;
  auto const fy = (anchor.y - before.origin.y) / before.extent.height;
  state.zoom_at(2, anchor, view, wide);
  auto const after = state.image_rect(view, wide);
  CHECK(state.zoom == 2);
  CHECK(after.origin.x + fx * after.extent.width == Approx(anchor.x));
  CHECK(after.origin.y + fy * after.extent.height == Approx(anchor.y));
}

TEST_CASE("the zoom stays between one and eight", "[zoom]") {
  zoom_state state;
  state.zoom_at(100, {400, 300}, view, wide);
  CHECK(state.zoom == 8);
  state.zoom_at(0.001F, {400, 300}, view, wide);
  CHECK(state.zoom == 1);
  CHECK(state.pan.x == 0);
  CHECK(state.pan.y == 0);
}

TEST_CASE("the image cannot be dragged off the view", "[zoom]") {
  zoom_state state;
  state.zoom_at(4, {400, 300}, view, wide);
  state.pan_by({100000, 100000}, view, wide);
  auto const rect = state.image_rect(view, wide);
  CHECK(rect.origin.x <= 0);
  CHECK(rect.origin.x + rect.extent.width >= view.width);
  CHECK(rect.origin.y <= 0);
  CHECK(rect.origin.y + rect.extent.height >= view.height);
  state.pan_by({-1000000, -1000000}, view, wide);
  auto const far = state.image_rect(view, wide);
  CHECK(far.origin.x + far.extent.width == Approx(view.width));
  CHECK(far.origin.y + far.extent.height == Approx(view.height));
}

TEST_CASE("an axis that fits cannot be panned", "[zoom]") {
  zoom_state state;
  // At 1.2 the wide image still fits the height: 400 × 1.2 is under 600.
  state.zoom_at(1.2F, {400, 300}, view, wide);
  state.pan_by({0, 500}, view, wide);
  CHECK(state.pan.y == 0);
  state.pan_by({500, 0}, view, wide);
  CHECK(state.pan.x == Approx(80));
}

TEST_CASE("reset returns to the fit", "[zoom]") {
  zoom_state state;
  state.zoom_at(3, {100, 100}, view, wide);
  state.reset();
  CHECK(state == zoom_state{});
}

TEST_CASE("a smaller view moves the limits", "[zoom]") {
  zoom_state state;
  state.zoom_at(2, {400, 300}, view, wide);
  state.pan_by({300, 0}, view, wide);
  state.clamp({400, 300}, wide);
  auto const fitted = zoom_state::fitted({400, 300}, wide);
  CHECK(state.pan.x <= (fitted.width * 2 - 400) / 2 + 0.001F);
}

TEST_CASE("the zoom view reacts to the wheel, a drag and a double click", "[zoom][widget]") {
  basalt::widget_tree tree{basalt::make_theme(basalt::color_scheme::light)};
  tree.set_viewport({800, 600});
  auto &zoom = tree.emplace_root<zoom_view>();
  zoom.set_source_size({4000, 2000});
  basalt::raster_image_ptr const image =
      basalt::raster_image::create(4, 2, std::vector<std::uint8_t>(4 * 2 * 4, 255));
  zoom.set_image(image);
  tree.update_layout();
  basalt::event_router router{tree};
  std::vector<float> zooms;
  zoom.set_on_zoom_change([&](float level) { zooms.push_back(level); });

  CHECK(zoom.zoom() == 1);
  router.dispatch(basalt::wheel_event{.position = {400, 300}, .delta = {0, -2}});
  CHECK(zoom.zoom() > 1);
  auto const zoomed = zoom.zoom();
  router.dispatch(basalt::wheel_event{.position = {400, 300}, .delta = {0, 2}});
  CHECK(zoom.zoom() < zoomed);

  zoom.reset_zoom();
  zoom.zoom_in();
  zoom.zoom_in();
  auto const before = zoom.state().pan;
  router.dispatch(basalt::pointer_event{.action = basalt::pointer_action::down,
                                        .position = {400, 300},
                                        .button = basalt::pointer_button::primary,
                                        .buttons = basalt::pointer_button::primary});
  router.dispatch(basalt::pointer_event{.action = basalt::pointer_action::move,
                                        .position = {350, 300},
                                        .buttons = basalt::pointer_button::primary});
  router.dispatch(basalt::pointer_event{.action = basalt::pointer_action::up,
                                        .position = {350, 300},
                                        .button = basalt::pointer_button::primary});
  CHECK(zoom.state().pan.x == before.x - 50);

  router.dispatch(basalt::pointer_event{.action = basalt::pointer_action::down,
                                        .position = {400, 300},
                                        .button = basalt::pointer_button::primary,
                                        .buttons = basalt::pointer_button::primary,
                                        .click_count = 2});
  CHECK(zoom.zoom() == 1);
  CHECK(zoom.state().pan.x == 0);
  CHECK_FALSE(zooms.empty());
}

TEST_CASE("a second image replaces the first without moving the view", "[zoom][widget]") {
  basalt::widget_tree tree{basalt::make_theme(basalt::color_scheme::light)};
  tree.set_viewport({800, 600});
  auto &zoom = tree.emplace_root<zoom_view>();
  zoom.set_source_size({4000, 2000});
  zoom.set_image(basalt::raster_image::create(4, 2, std::vector<std::uint8_t>(32, 0)));
  tree.update_layout();
  zoom.zoom_in();
  auto const state = zoom.state();
  zoom.set_image(basalt::raster_image::create(40, 20, std::vector<std::uint8_t>(3200, 0)));
  CHECK(zoom.state() == state);
  CHECK(zoom.image()->width() == 40);
}
