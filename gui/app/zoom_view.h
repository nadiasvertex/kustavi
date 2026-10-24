#pragma once

#include "basalt/basalt.h"

#include <functional>

namespace kustavi::gui {

// Where an image sits in a view that can zoom and pan. The image is fitted into the view at zoom
// one, and each zoom step enlarges it around a point. A value type, so the rules are testable
// without a window.
struct zoom_state {
  static constexpr float min_zoom = 1;
  static constexpr float max_zoom = 8;

  float zoom = 1;
  // How far the middle of the image is from the middle of the view.
  basalt::point pan{0, 0};

  // The size of the image at zoom one: the largest that fits the view with its aspect kept.
  [[nodiscard]] static auto fitted(basalt::size view, basalt::size source) -> basalt::size;
  // The rectangle of the image in view coordinates.
  [[nodiscard]] auto image_rect(basalt::size view, basalt::size source) const -> basalt::rect;

  // Multiplies the zoom, within the limits, so that the image point under `anchor` stays there.
  auto zoom_at(float factor, basalt::point anchor, basalt::size view, basalt::size source) -> void;
  auto pan_by(basalt::point delta, basalt::size view, basalt::size source) -> void;
  // Keeps the image over the view: an image smaller than the view on one axis is centered on it, and
  // a larger one cannot leave a gap.
  auto clamp(basalt::size view, basalt::size source) -> void;
  auto reset() -> void;

  [[nodiscard]] friend auto operator==(zoom_state const &, zoom_state const &) -> bool = default;
};

// Shows one image that the user can zoom with the wheel, pan by dragging, and reset by a double
// click. The picture can be swapped for a sharper one of the same photo, such as the full-size
// master for its preview, without moving the view.
class zoom_view : public basalt::widget {
public:
  zoom_view() = default;

  // The size of the picture that the image stands for, which fixes its shape. When it is empty the
  // size of the raster is used.
  auto set_source_size(basalt::size size) -> void;
  auto set_image(basalt::raster_image_ptr image) -> void;
  [[nodiscard]] auto image() const -> basalt::raster_image_ptr const & { return image_; }

  [[nodiscard]] auto state() const -> zoom_state const & { return state_; }
  auto zoom_in() -> void;
  auto zoom_out() -> void;
  auto reset_zoom() -> void;
  [[nodiscard]] auto zoom() const -> float { return state_.zoom; }
  // The size that the view asks for when its parent does not limit it.
  auto set_preferred_size(basalt::size size) -> void {
    preferred_ = size;
    invalidate_layout();
  }
  // Called after the zoom changes.
  auto set_on_zoom_change(std::function<void(float)> handler) -> void {
    on_zoom_change_ = std::move(handler);
  }
  // The rectangle of the image in this widget's coordinates.
  [[nodiscard]] auto image_rect() const -> basalt::rect;

protected:
  auto on_event(basalt::event_context &context, basalt::event const &e) -> bool override;
  auto on_paint(basalt::render_context &context) const -> void override;
  [[nodiscard]] auto on_measure(basalt::constraints const &limits) const
      -> basalt::size override;
  auto on_layout() -> void override;

private:
  [[nodiscard]] auto source() const -> basalt::size;
  [[nodiscard]] auto view_size() const -> basalt::size { return bounds().extent; }
  auto changed() -> void;

  basalt::raster_image_ptr image_;
  basalt::size source_size_;
  basalt::size preferred_{400, 300};
  zoom_state state_;
  bool dragging_ = false;
  basalt::point last_;
  std::function<void(float)> on_zoom_change_;
};

} // namespace kustavi::gui
