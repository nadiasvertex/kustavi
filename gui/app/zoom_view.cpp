#include "app/zoom_view.h"

#include <algorithm>
#include <cmath>

namespace kustavi::gui {

auto zoom_state::fitted(basalt::size view, basalt::size source) -> basalt::size {
  if (view.width <= 0 || view.height <= 0 || source.width <= 0 || source.height <= 0) {
    return {};
  }
  auto const scale = std::min(view.width / source.width, view.height / source.height);
  return {source.width * scale, source.height * scale};
}

auto zoom_state::image_rect(basalt::size view, basalt::size source) const -> basalt::rect {
  auto const base = fitted(view, source);
  basalt::size const size{base.width * zoom, base.height * zoom};
  basalt::point const center{view.width / 2 + pan.x, view.height / 2 + pan.y};
  return {{center.x - size.width / 2, center.y - size.height / 2}, size};
}

auto zoom_state::clamp(basalt::size view, basalt::size source) -> void {
  auto const base = fitted(view, source);
  auto const limit = [](float shown, float room) {
    return shown <= room ? 0.0F : (shown - room) / 2;
  };
  auto const x = limit(base.width * zoom, view.width);
  auto const y = limit(base.height * zoom, view.height);
  pan.x = std::clamp(pan.x, -x, x);
  pan.y = std::clamp(pan.y, -y, y);
}

auto zoom_state::zoom_at(float factor, basalt::point anchor, basalt::size view,
                         basalt::size source) -> void {
  auto const next = std::clamp(zoom * factor, min_zoom, max_zoom);
  if (next == zoom) {
    return;
  }
  // The point of the image under the anchor, in units of the current scale.
  basalt::point const center{view.width / 2 + pan.x, view.height / 2 + pan.y};
  auto const ratio = next / zoom;
  basalt::point const moved{anchor.x - (anchor.x - center.x) * ratio,
                            anchor.y - (anchor.y - center.y) * ratio};
  zoom = next;
  pan = {moved.x - view.width / 2, moved.y - view.height / 2};
  clamp(view, source);
}

auto zoom_state::pan_by(basalt::point delta, basalt::size view, basalt::size source) -> void {
  pan = {pan.x + delta.x, pan.y + delta.y};
  clamp(view, source);
}

auto zoom_state::reset() -> void {
  zoom = 1;
  pan = {0, 0};
}

// ---- The widget ----

auto zoom_view::source() const -> basalt::size {
  if (source_size_.width > 0 && source_size_.height > 0) {
    return source_size_;
  }
  if (image_ != nullptr) {
    return {static_cast<float>(image_->width()), static_cast<float>(image_->height())};
  }
  return {};
}

auto zoom_view::set_source_size(basalt::size size) -> void {
  source_size_ = size;
  state_.clamp(view_size(), source());
  invalidate_paint();
}

auto zoom_view::set_image(basalt::raster_image_ptr image) -> void {
  image_ = std::move(image);
  state_.clamp(view_size(), source());
  invalidate_paint();
}

auto zoom_view::image_rect() const -> basalt::rect {
  return state_.image_rect(view_size(), source());
}

auto zoom_view::changed() -> void {
  invalidate_paint();
  if (on_zoom_change_) {
    on_zoom_change_(state_.zoom);
  }
}

auto zoom_view::zoom_in() -> void {
  auto const before = state_.zoom;
  state_.zoom_at(1.25F, {view_size().width / 2, view_size().height / 2}, view_size(), source());
  if (state_.zoom != before) {
    changed();
  }
}

auto zoom_view::zoom_out() -> void {
  auto const before = state_.zoom;
  state_.zoom_at(0.8F, {view_size().width / 2, view_size().height / 2}, view_size(), source());
  if (state_.zoom != before) {
    changed();
  }
}

auto zoom_view::reset_zoom() -> void {
  auto const before = state_;
  state_.reset();
  if (!(state_ == before)) {
    changed();
  }
}

auto zoom_view::on_layout() -> void {
  // A new size changes how far the image may move.
  state_.clamp(view_size(), source());
}

auto zoom_view::on_measure(basalt::constraints const &limits) const -> basalt::size {
  return limits.constrain({limits.bounded_width() ? limits.max.width : preferred_.width,
                           limits.bounded_height() ? limits.max.height : preferred_.height});
}

auto zoom_view::on_paint(basalt::render_context &context) const -> void {
  if (image_ == nullptr) {
    return;
  }
  auto const clip = context.clip({{0, 0}, view_size()});
  context.draw_image(image_rect(), image_);
}

auto zoom_view::on_event(basalt::event_context &, basalt::event const &e) -> bool {
  if (auto const *wheel = std::get_if<basalt::wheel_event>(&e)) {
    if (image_ == nullptr || wheel->delta.y == 0) {
      return false;
    }
    // Scrolling up zooms in. A notch is a step, and a smooth scroll counts by its distance.
    auto const amount = wheel->precise ? wheel->delta.y * 0.01F : wheel->delta.y * 0.2F;
    auto const before = state_.zoom;
    state_.zoom_at(std::exp(-amount), wheel->position, view_size(), source());
    if (state_.zoom != before) {
      changed();
    }
    return true;
  }
  auto const *pointer = std::get_if<basalt::pointer_event>(&e);
  if (pointer == nullptr) {
    return false;
  }
  switch (pointer->action) {
  case basalt::pointer_action::down:
    if (pointer->button != basalt::pointer_button::primary) {
      return false;
    }
    if (pointer->click_count >= 2) {
      reset_zoom();
      return true;
    }
    dragging_ = true;
    last_ = pointer->position;
    return true;
  case basalt::pointer_action::move:
    if (!dragging_) {
      return false;
    }
    state_.pan_by({pointer->position.x - last_.x, pointer->position.y - last_.y}, view_size(),
                  source());
    last_ = pointer->position;
    invalidate_paint();
    return true;
  case basalt::pointer_action::up:
  case basalt::pointer_action::cancel:
    if (!dragging_) {
      return false;
    }
    dragging_ = false;
    return true;
  default:
    return false;
  }
}

} // namespace kustavi::gui
