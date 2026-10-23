#pragma once

#include "basalt/basalt.h"

#include <memory>
#include <string>
#include <utility>

namespace kustavi::gui::ui {

// Small builders that the screens share. They make widgets and hand back ownership.

inline auto column(float gap = 8) -> std::unique_ptr<basalt::box> {
  auto box = std::make_unique<basalt::box>(basalt::axis::vertical);
  box->set_gap(basalt::fixed(gap));
  return box;
}

inline auto row(float gap = 8) -> std::unique_ptr<basalt::box> {
  auto box = std::make_unique<basalt::box>(basalt::axis::horizontal);
  box->set_gap(basalt::fixed(gap));
  box->set_cross_alignment(basalt::alignment::center);
  return box;
}

inline auto text(std::string value, basalt::text_role role = basalt::text_role::body,
                 basalt::text_tone tone = basalt::text_tone::normal)
    -> std::unique_ptr<basalt::label> {
  auto label = std::make_unique<basalt::label>(std::move(value), role);
  label->set_tone(tone);
  return label;
}

// A column that centers its children on both axes and fills the space it is given.
inline auto centered(float gap = 8) -> std::unique_ptr<basalt::box> {
  auto box = column(gap);
  box->set_main_alignment(basalt::alignment::center);
  box->set_cross_alignment(basalt::alignment::center);
  return box;
}

// Adds `child` so that it takes the space left over.
template <std::derived_from<basalt::widget> T>
auto fill(basalt::box &parent, std::unique_ptr<T> child) -> T & {
  return parent.add(std::move(child), basalt::box_item{.grow = 1});
}

inline auto make_button(std::string label, std::function<void()> on_press,
                        basalt::button_variant variant = basalt::button_variant::secondary)
    -> std::unique_ptr<basalt::button> {
  auto button = std::make_unique<basalt::button>(std::move(label), std::move(on_press));
  button->set_variant(variant);
  return button;
}

} // namespace kustavi::gui::ui
