#pragma once

#include "app/image_loader.h"
#include "core/domain.h"

#include "basalt/basalt.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace kustavi::gui {

// A grid of the working images of a list of photos. It makes the grid widget, asks the loader for
// the images in view and gives the finished ones to the cells. It lives as long as the screen that
// shows it, and the widget it made must not outlive it.
class image_grid {
public:
  // `images` are shown in order. `loader` outlives the grid.
  image_grid(std::vector<image_info> images, image_loader &loader);

  // The widget to put in a screen. The grid keeps a pointer to it, so call this once.
  [[nodiscard]] auto make_widget() -> std::unique_ptr<basalt::grid_view>;
  // Gives the loader's finished images to the cells. Call it on the UI thread every frame.
  auto apply_ready_images() -> void;

  [[nodiscard]] auto view() const -> basalt::grid_view * { return view_; }
  [[nodiscard]] auto item_count() const -> std::size_t { return images_.size(); }

  static constexpr float cell_edge = 160;

private:
  auto make_cell(std::size_t index, std::unique_ptr<basalt::widget> recycled)
      -> std::unique_ptr<basalt::widget>;
  auto request_images() -> void;
  auto trim_images() -> void;

  std::vector<image_info> images_;
  image_loader &loader_;
  basalt::grid_view *view_ = nullptr;
  // The decoded images, by the photo's id. The cells near the view keep theirs.
  std::unordered_map<std::string, basalt::raster_image_ptr> decoded_;
};

} // namespace kustavi::gui
