#pragma once

#include "app/wrapped_text.h"

#include "basalt/basalt.h"

#include <functional>
#include <string>

namespace kustavi::gui {

// One batch in the batch menu's list: its title, wrapped when it is long, and a quieter line of
// counts below it. The selected batch has a tinted background and an accent bar at its start
// edge. A click, Enter or Space selects it.
class batch_list_item : public basalt::widget {
public:
  batch_list_item(std::string title, std::string details, bool selected,
                  std::function<void()> on_press);

  [[nodiscard]] auto title() const -> std::string const & { return title_.text(); }
  [[nodiscard]] auto details() const -> std::string const & { return details_.text(); }
  [[nodiscard]] auto selected() const -> bool { return selected_; }

  // Calls the handler as a click would. False when the item is disabled.
  auto press() -> bool;

  [[nodiscard]] auto accessibility_info() const -> basalt::accessibility::info override;

protected:
  auto on_event(basalt::event_context &context, basalt::event const &e) -> bool override;
  auto on_paint(basalt::render_context &context) const -> void override;
  [[nodiscard]] auto on_measure(basalt::constraints const &limits) const -> basalt::size override;
  auto on_accessibility_action(basalt::accessibility::action_request const &request)
      -> bool override;

private:
  [[nodiscard]] auto padding() const -> basalt::insets;

  wrapped_text title_;
  wrapped_text details_;
  bool selected_;
  std::function<void()> on_press_;
};

} // namespace kustavi::gui
