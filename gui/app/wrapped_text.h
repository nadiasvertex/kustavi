#pragma once

#include "basalt/basalt.h"
#include "basalt/text/paragraph.h"

#include <memory>
#include <string>

namespace kustavi::gui {

// Text that wraps onto as many lines as a width needs, for strings such as trip names that can be
// longer than the space they are shown in. Like `basalt::text_line`, it keeps its layout until the
// text, role, theme, scale or width changes.
class wrapped_text {
public:
  wrapped_text() = default;
  explicit wrapped_text(std::string text, basalt::text_role role = basalt::text_role::body)
      : text_(std::move(text)), role_(role) {}

  [[nodiscard]] auto text() const -> std::string const & { return text_; }
  // False when the text did not change.
  auto set_text(std::string text) -> bool;
  [[nodiscard]] auto role() const -> basalt::text_role { return role_; }
  auto set_role(basalt::text_role role) -> bool;

  // The size of the text wrapped at `width`; an unbounded width keeps it on one line.
  [[nodiscard]] auto measure(basalt::widget const &owner, float width) const -> basalt::size;
  // Draws the text wrapped at `width` with the top left of its first line at `origin`.
  auto draw(basalt::render_context &context, basalt::widget const &owner, basalt::point origin,
            float width, basalt::rgba color) const -> void;

private:
  struct cache {
    basalt::text::text_shaper const *shaper = nullptr;
    basalt::text_style style;
    float width = -1;
    // Cleared by `set_text` and `set_role`, the only mutators of the input.
    bool valid = false;
    std::shared_ptr<basalt::text::paragraph const> paragraph;
    float line_height = 0;
  };

  auto laid_out(basalt::widget const &owner, float width) const -> cache const &;

  std::string text_;
  basalt::text_role role_ = basalt::text_role::body;
  mutable cache cache_;
};

// A label whose text wraps at the width it is given instead of being clipped.
class wrapped_label : public basalt::widget {
public:
  explicit wrapped_label(std::string text = {}, basalt::text_role role = basalt::text_role::body,
                         basalt::text_tone tone = basalt::text_tone::normal)
      : text_(std::move(text), role), tone_(tone) {}

  [[nodiscard]] auto text() const -> std::string const & { return text_.text(); }
  auto set_text(std::string text) -> void;

  [[nodiscard]] auto accessibility_info() const -> basalt::accessibility::info override;

protected:
  auto on_paint(basalt::render_context &context) const -> void override;
  [[nodiscard]] auto on_measure(basalt::constraints const &limits) const -> basalt::size override;

private:
  wrapped_text text_;
  basalt::text_tone tone_;
};

} // namespace kustavi::gui
