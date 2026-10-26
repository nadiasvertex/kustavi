#include "app/batch_list_item.h"

#include <algorithm>
#include <utility>
#include <variant>

namespace kustavi::gui {

namespace {

// The space between the title and the line of counts.
constexpr float details_gap = 2;
// The width of the bar that marks the selected batch.
constexpr float selection_bar = 3;

} // namespace

batch_list_item::batch_list_item(std::string title, std::string details, bool selected,
                                 std::function<void()> on_press)
    : title_(std::move(title), basalt::text_role::body_strong),
      details_(std::move(details), basalt::text_role::small), selected_(selected),
      on_press_(std::move(on_press)) {
  set_focus_policy(basalt::focus_policy::strong);
  set_minimum_target(true);
}

auto batch_list_item::press() -> bool {
  if (!enabled_in_tree()) {
    return false;
  }
  // The handler may rebuild the screen and destroy this item, so it runs from a copy.
  if (auto const handler = on_press_) {
    handler();
  }
  return true;
}

auto batch_list_item::accessibility_info() const -> basalt::accessibility::info {
  basalt::accessibility::info details;
  details.kind = basalt::accessibility::role::list_item;
  details.name = title_.text();
  details.description = details_.text();
  details.selected = selected_;
  details.actions = {basalt::accessibility::action::press, basalt::accessibility::action::focus};
  return details;
}

auto batch_list_item::on_accessibility_action(
    basalt::accessibility::action_request const &request) -> bool {
  return request.kind == basalt::accessibility::action::press && press();
}

auto batch_list_item::on_event(basalt::event_context &, basalt::event const &e) -> bool {
  if (auto const *pointer = std::get_if<basalt::pointer_event>(&e)) {
    if (pointer->button != basalt::pointer_button::primary) {
      return false;
    }
    if (pointer->action == basalt::pointer_action::up && interaction().pressed) {
      press();
      return true;
    }
    return pointer->action == basalt::pointer_action::down ||
           pointer->action == basalt::pointer_action::up;
  }
  if (auto const *key = std::get_if<basalt::key_event>(&e)) {
    if (key->pressed && !key->repeat && key->modifiers == basalt::modifier::none &&
        (key->code == basalt::key::space || key->code == basalt::key::enter)) {
      press();
      return true;
    }
  }
  return false;
}

auto batch_list_item::padding() const -> basalt::insets {
  auto const &sizes = theme().sizes;
  auto const horizontal = resolve(sizes.inset_horizontal_list_item);
  auto const vertical = resolve(basalt::fixed(10));
  return {vertical, horizontal, vertical, horizontal + resolve(basalt::fixed(selection_bar))};
}

auto batch_list_item::on_measure(basalt::constraints const &limits) const -> basalt::size {
  auto const pad = padding();
  auto const width = limits.bounded_width()
                         ? std::max(0.0F, limits.max.width - pad.left - pad.right)
                         : basalt::unbounded;
  auto const title = title_.measure(*this, width);
  auto const details = details_.measure(*this, width);
  return {std::max(title.width, details.width) + pad.left + pad.right,
          title.height + resolve(basalt::fixed(details_gap)) + details.height + pad.top +
              pad.bottom};
}

auto batch_list_item::on_paint(basalt::render_context &context) const -> void {
  if (tree() == nullptr) {
    return;
  }
  auto const &t = theme();
  auto const &c = t.colors;
  basalt::rect const area{{}, bounds().extent};
  auto const radius = resolve(t.sizes.radius_default);
  auto const state = basalt::control_state_of(interaction(), enabled_in_tree());
  auto const fill = selected_ ? basalt::by_state(state, c.background_accent_subdued,
                                                 c.background_accent_subdued_hover,
                                                 c.background_accent_subdued_active)
                              : basalt::by_state(state, basalt::rgba{0, 0, 0, 0},
                                                 c.background_neutral_hover,
                                                 c.background_neutral_active);
  if (fill.a != 0) {
    context.fill_rounded_rect(area, radius, fill);
  }
  if (selected_) {
    auto const bar = resolve(basalt::fixed(selection_bar));
    context.fill_rounded_rect({{0, 0}, {bar, area.height()}}, bar / 2, c.border_accent);
  }

  auto const pad = padding();
  auto const width = std::max(0.0F, area.width() - pad.left - pad.right);
  title_.draw(context, *this, {pad.left, pad.top}, width,
              selected_ ? c.text_interactive : c.text_default);
  auto const below = pad.top + title_.measure(*this, width).height +
                     resolve(basalt::fixed(details_gap));
  details_.draw(context, *this, {pad.left, below}, width, c.text_subdued);
}

} // namespace kustavi::gui
