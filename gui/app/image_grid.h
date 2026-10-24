#pragma once

#include "app/image_loader.h"
#include "core/domain.h"

#include "basalt/basalt.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace kustavi::gui {

// The decoded images that the screens share, by the photo's id. A screen that is built again after
// an edit finds its images here and does not decode them twice.
using decoded_image_cache = std::unordered_map<std::string, basalt::raster_image_ptr>;

// A run of photos in a grid. A group with a header gets a full-width row above its photos, and
// may have no photos.
struct image_group {
  std::vector<image_info> images;
  bool header = false;
};

// A grid of the working images of groups of photos. It makes the grid widget, asks the loader for
// the images in view and gives the finished ones to the cells. It lives as long as the screen that
// shows it, and the widget it made must not outlive it.
class image_grid {
public:
  // `loader` and `cache` outlive the grid.
  image_grid(std::vector<image_group> groups, image_loader &loader, decoded_image_cache &cache);
  // One group without a header.
  image_grid(std::vector<image_info> images, image_loader &loader, decoded_image_cache &cache);

  // Builds the header widget of a section. Set it before `make_widget`.
  auto set_header_factory(std::function<std::unique_ptr<basalt::widget>(std::size_t section)> f)
      -> void {
    header_factory_ = std::move(f);
  }
  // Changes a cell after the image is set, such as to show that it is selected.
  auto set_cell_decorator(std::function<void(basalt::image_tile &, image_info const &)> d)
      -> void {
    decorator_ = std::move(d);
  }
  // Puts a small button in the top right corner of each cell that opens the photo.
  auto set_on_open(std::function<void(image_info const &)> handler) -> void {
    on_open_ = std::move(handler);
  }
  // Called with the photo that the user activates.
  auto set_on_activate(std::function<void(image_info const &)> handler) -> void {
    on_activate_ = std::move(handler);
  }

  // The widget to put in a screen. The grid keeps a pointer to it, so call this once.
  [[nodiscard]] auto make_widget() -> std::unique_ptr<basalt::grid_view>;
  // Gives the loader's finished images to the cells. Call it on the UI thread every frame.
  auto apply_ready_images() -> void;
  // Builds the cells in view again, after the decorator's state changed.
  auto reload() -> void;

  [[nodiscard]] auto view() const -> basalt::grid_view * { return view_; }
  [[nodiscard]] auto item_count() const -> std::size_t { return images_.size(); }

  static constexpr float cell_edge = 160;
  static constexpr float header_height = 40;

private:
  auto make_cell(std::size_t index, std::unique_ptr<basalt::widget> recycled)
      -> std::unique_ptr<basalt::widget>;
  auto request_images() -> void;
  auto trim_images() -> void;

  std::vector<image_info> images_;
  std::vector<basalt::grid_section> sections_;
  image_loader &loader_;
  decoded_image_cache &decoded_;
  basalt::grid_view *view_ = nullptr;
  std::function<std::unique_ptr<basalt::widget>(std::size_t)> header_factory_;
  std::function<void(basalt::image_tile &, image_info const &)> decorator_;
  std::function<void(image_info const &)> on_activate_;
  std::function<void(image_info const &)> on_open_;
};

} // namespace kustavi::gui
