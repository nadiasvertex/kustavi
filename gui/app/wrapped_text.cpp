#include "app/wrapped_text.h"

#include <algorithm>
#include <utility>

namespace kustavi::gui {

auto wrapped_text::set_text(std::string text) -> bool {
  if (text == text_) {
    return false;
  }
  text_ = std::move(text);
  cache_.valid = false;
  return true;
}

auto wrapped_text::set_role(basalt::text_role role) -> bool {
  if (role == role_) {
    return false;
  }
  role_ = role;
  cache_.valid = false;
  return true;
}

auto wrapped_text::laid_out(basalt::widget const &owner, float width) const -> cache const & {
  basalt::widget_tree const *const tree = owner.tree();
  basalt::text::text_shaper const *const shaper = tree != nullptr ? tree->text_shaper() : nullptr;
  auto const style = basalt::to_layout(basalt::text_style_for(owner.theme(), role_), owner.scale());
  if (cache_.valid && cache_.shaper == shaper && cache_.style == style && cache_.width == width) {
    return cache_;
  }
  cache_ = {.shaper = shaper, .style = style, .width = width, .valid = true};
  if (shaper != nullptr && !text_.empty()) {
    basalt::text::paragraph_options options;
    options.shape.style = style;
    options.max_width = width;
    cache_.paragraph = std::make_shared<basalt::text::paragraph const>(*shaper, text_, options);
  }
  cache_.line_height = style.line_height > 0 ? style.line_height : style.font_size * 1.25F;
  return cache_;
}

auto wrapped_text::measure(basalt::widget const &owner, float width) const -> basalt::size {
  auto const &c = laid_out(owner, width);
  if (c.paragraph == nullptr) {
    return {0, c.line_height};
  }
  auto const extent = c.paragraph->extent();
  return {extent.width, std::max(extent.height, c.line_height)};
}

auto wrapped_text::draw(basalt::render_context &context, basalt::widget const &owner,
                        basalt::point origin, float width, basalt::rgba color) const -> void {
  auto const &c = laid_out(owner, width);
  if (c.paragraph == nullptr) {
    return;
  }
  for (auto const &line : c.paragraph->lines()) {
    for (auto const &placed : line.runs) {
      context.draw_glyph_run({origin.x + placed.x, origin.y + line.y + line.baseline}, placed.run,
                             color);
    }
  }
}

auto wrapped_label::set_text(std::string text) -> void {
  if (text_.set_text(std::move(text))) {
    invalidate_content();
  }
}

auto wrapped_label::accessibility_info() const -> basalt::accessibility::info {
  return {.kind = basalt::accessibility::role::label, .name = text_.text()};
}

auto wrapped_label::on_paint(basalt::render_context &context) const -> void {
  text_.draw(context, *this, {0, 0}, bounds().width(), basalt::text_color_for(theme(), tone_));
}

auto wrapped_label::on_measure(basalt::constraints const &limits) const -> basalt::size {
  return text_.measure(*this, limits.max.width);
}

} // namespace kustavi::gui
