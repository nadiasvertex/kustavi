#pragma once

#include "app/image_grid.h"
#include "core/phases.h"
#include "core/wizard_controller.h"

#include "basalt/basalt.h"

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace kustavi::gui {

// What the trips review remembers while the screen is built again after each edit.
struct trips_view_state {
  bool settings_open = false;
  bool selecting = false;
  std::set<std::string> selected;
  // The folder whose name is being edited.
  std::optional<std::string> renaming_folder;
  // The settings sliders, in the units of the sliders.
  float gap_hours = 48;
  float distance_km = 300;
  float home_radius_km = 15;
  float leg_radius_km = 25;
  bool seeded = false;
  float scroll = 0;
};

// The trips review (S3): the clustering settings, the folders with their trips and legs as groups of
// photos, the photos in no trip, and the tools to move photos between trips. The groups are the
// sections of one grid, so the screen scrolls as a whole and a collection of any size stays light.
class trips_screen {
public:
  trips_screen(wizard_controller &controller, trips_view_state &state, image_loader &loader,
               decoded_image_cache &cache);
  trips_screen(trips_screen const &) = delete;
  auto operator=(trips_screen const &) -> trips_screen & = delete;

  [[nodiscard]] auto build(wizard_trips_review const &review) -> std::unique_ptr<basalt::widget>;
  // Rebuilds the toolbar and the settings after the state changed, and gives finished images to
  // the cells. Call it every frame, before layout.
  auto update() -> void;
  // Keeps the scroll position for the next screen that is built.
  auto remember_scroll() -> void;

  // The text of the line above the toolbar, such as "2 folders · 3 trips · 1 marked".
  [[nodiscard]] static auto summary_text(wizard_trips_review const &review) -> std::string;

private:
  enum class header_kind { folder, trip, leg, unassigned };
  struct header_info {
    header_kind kind = header_kind::trip;
    std::string folder_name;
    trip_info trip;
    trip_leg_info leg;
    int folder_trips = 0;
    int photos = 0;
  };

  auto make_header(std::size_t section) -> std::unique_ptr<basalt::widget>;
  auto build_toolbar() -> void;
  auto build_settings() -> void;
  auto open_move_menu(basalt::widget &anchor) -> void;
  auto toggle_selection(image_info const &image) -> void;
  auto leave_selection() -> void;
  auto retire_children(basalt::box &box) -> void;

  wizard_controller &controller_;
  trips_view_state &state_;
  image_loader &loader_;
  decoded_image_cache &cache_;
  std::vector<header_info> headers_;
  std::vector<trip_info> trips_;
  std::vector<trip_folder_info> folders_;
  std::unique_ptr<image_grid> grid_;
  basalt::box *toolbar_ = nullptr;
  basalt::box *settings_ = nullptr;
  // Widgets taken out of the tree during a rebuild. They go at the next one, after the handler that
  // was running when they were removed has returned.
  std::vector<std::unique_ptr<basalt::widget>> retired_;
  bool controls_dirty_ = false;
  bool headers_dirty_ = false;
};

} // namespace kustavi::gui
