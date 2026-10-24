#include "app/image_grid.h"

#include <algorithm>
#include <iterator>

namespace kustavi::gui {

namespace {

// How many decoded images the grid keeps beyond the realized cells, for scrolling back.
constexpr std::size_t kept_images = 1'500;

} // namespace

image_grid::image_grid(std::vector<image_group> groups, image_loader &loader,
                       decoded_image_cache &cache)
    : loader_(loader), decoded_(cache) {
  for (auto &group : groups) {
    sections_.push_back({group.images.size(), group.header});
    std::ranges::move(group.images, std::back_inserter(images_));
  }
}

image_grid::image_grid(std::vector<image_info> images, image_loader &loader,
                       decoded_image_cache &cache)
    : image_grid(std::vector<image_group>{{std::move(images), false}}, loader, cache) {}

auto image_grid::make_widget() -> std::unique_ptr<basalt::grid_view> {
  auto view = std::make_unique<basalt::grid_view>("Photos");
  view_ = view.get();
  view->set_cell_size(basalt::fixed(cell_edge), basalt::fixed(cell_edge));
  view->set_gap(basalt::fixed(8));
  view->set_cell_factory([this](std::size_t index, std::unique_ptr<basalt::widget> recycled) {
    return make_cell(index, std::move(recycled));
  });
  view->set_on_visible_change([this](basalt::index_range) { request_images(); });
  view->set_header_height(basalt::fixed(header_height));
  if (header_factory_) {
    view->set_header_factory(header_factory_);
  }
  if (on_activate_) {
    view->set_on_activate([this](std::size_t item) {
      if (item < images_.size()) {
        on_activate_(images_[item]);
      }
    });
  }
  view->set_sections(sections_);
  return view;
}

auto image_grid::make_cell(std::size_t index, std::unique_ptr<basalt::widget> recycled)
    -> std::unique_ptr<basalt::widget> {
  std::unique_ptr<basalt::image_tile> tile;
  if (recycled != nullptr) {
    tile.reset(static_cast<basalt::image_tile *>(recycled.release()));
  } else {
    tile = std::make_unique<basalt::image_tile>();
  }
  auto const &info = images_[index];
  tile->set_caption(info.name);
  auto const known = decoded_.find(info.id);
  tile->set_image(known != decoded_.end() ? known->second : nullptr);
  tile->set_selected(false);
  if (decorator_) {
    decorator_(*tile, info);
  }
  if (on_open_) {
    auto open = std::make_unique<basalt::button>("⤢", [this, index] { on_open_(images_[index]); });
    open->set_variant(basalt::button_variant::tertiary);
    tile->set_badge(basalt::tile_corner::top_right, std::move(open));
  }
  return tile;
}

// Asks for the images of the cells that have none, the ones in view first.
auto image_grid::request_images() -> void {
  if (view_ == nullptr) {
    return;
  }
  auto const visible = view_->visible_items();
  auto const realized = view_->realized_items();
  std::vector<image_request> wanted;
  auto const add = [&](std::size_t from, std::size_t to) {
    for (std::size_t item = from; item < to && item < images_.size(); ++item) {
      auto const &info = images_[item];
      if (!decoded_.contains(info.id) && !info.working_image_path.empty()) {
        wanted.push_back({info.id, info.working_image_path});
      }
    }
  };
  add(visible.first, visible.last);
  add(realized.first, visible.first);
  add(visible.last, realized.last);
  loader_.request(this, std::move(wanted));
}

auto image_grid::apply_ready_images() -> void {
  auto ready = loader_.take_ready();
  if (ready.empty() || view_ == nullptr) {
    return;
  }
  // Positions by id, to find the cell of a finished image.
  std::unordered_map<std::string_view, std::size_t> position;
  auto const realized = view_->realized_items();
  for (std::size_t item = realized.first; item < realized.last && item < images_.size(); ++item) {
    position.emplace(images_[item].id, item);
  }
  for (auto &[key, image] : ready) {
    if (auto const at = position.find(key); at != position.end()) {
      if (auto *const cell = view_->cell(at->second)) {
        static_cast<basalt::image_tile *>(cell)->set_image(image);
      }
    }
    decoded_[key] = std::move(image);
  }
  trim_images();
}

auto image_grid::reload() -> void {
  if (view_ != nullptr) {
    view_->reload();
  }
}

auto image_grid::trim_images() -> void {
  if (decoded_.size() <= kept_images) {
    return;
  }
  std::unordered_map<std::string_view, bool> near;
  auto const realized = view_->realized_items();
  for (std::size_t item = realized.first; item < realized.last && item < images_.size(); ++item) {
    near.emplace(images_[item].id, true);
  }
  std::erase_if(decoded_, [&](auto const &entry) { return !near.contains(entry.first); });
}

} // namespace kustavi::gui
