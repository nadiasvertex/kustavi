#include "app/trips_screen.h"

#include "app/ui_kit.h"
#include "core/format.h"

#include <algorithm>
#include <format>

namespace kustavi::gui {

namespace {

using basalt::button_variant;
using basalt::text_role;
using basalt::text_tone;

auto iso_date(time_ms instant) -> std::string {
  auto const civil = to_local_civil(instant);
  return std::format("{:04}-{:02}-{:02}", civil.year, civil.month, civil.day);
}

auto trip_label(trip_info const &trip) -> std::string {
  if (trip.is_home) {
    return "Home";
  }
  if (!trip.place_name.empty()) {
    return trip.place_name;
  }
  return trip.folder.value_or("Trip");
}

auto plural(std::size_t count, char const *one, char const *many) -> std::string {
  return std::format("{} {}", count, count == 1 ? one : many);
}

} // namespace

trips_screen::trips_screen(wizard_controller &controller, trips_view_state &state,
                           image_loader &loader, decoded_image_cache &cache)
    : controller_(controller), state_(state), loader_(loader), cache_(cache) {}

auto trips_screen::summary_text(wizard_trips_review const &review) -> std::string {
  auto text = std::format("{} · {} · {} marked", plural(review.folders.size(), "folder", "folders"),
                          plural(review.trips.size(), "trip", "trips"), review.marked_count);
  if (!review.unassigned_ids.empty()) {
    text += std::format(" · {} unassigned", review.unassigned_ids.size());
  }
  return text;
}

auto trips_screen::retire_children(basalt::box &box) -> void {
  while (box.child_count() > 0) {
    retired_.push_back(box.remove_child(box.child(0)));
  }
}

auto trips_screen::build(wizard_trips_review const &review) -> std::unique_ptr<basalt::widget> {
  if (!state_.seeded) {
    auto const &tunables = controller_.tunables();
    state_.gap_hours = static_cast<float>(tunables.trip_gap_hours);
    state_.distance_km = static_cast<float>(tunables.trip_distance_km);
    state_.home_radius_km = static_cast<float>(tunables.trip_home_radius_km);
    state_.leg_radius_km = static_cast<float>(tunables.trip_leg_radius_km);
    state_.seeded = true;
  }
  trips_ = review.trips;
  folders_ = review.folders;

  auto const images_of = [&](std::vector<std::string> const &ids) {
    std::vector<image_info> images;
    images.reserve(ids.size());
    for (auto const &id : ids) {
      if (auto const *image = controller_.image(id)) {
        images.push_back(*image);
      }
    }
    return images;
  };

  headers_.clear();
  std::vector<image_group> groups;
  for (auto const &folder : review.folders) {
    header_info folder_header;
    folder_header.kind = header_kind::folder;
    folder_header.folder_name = folder.name;
    folder_header.folder_trips = static_cast<int>(folder.trips.size());
    headers_.push_back(std::move(folder_header));
    groups.push_back({{}, true});
    for (auto const &trip : folder.trips) {
      header_info trip_header;
      trip_header.kind = header_kind::trip;
      trip_header.trip = trip;
      trip_header.photos = static_cast<int>(trip.member_ids.size());
      headers_.push_back(std::move(trip_header));
      if (trip.legs.size() <= 1) {
        groups.push_back({images_of(trip.member_ids), true});
        continue;
      }
      groups.push_back({{}, true});
      for (auto const &leg : trip.legs) {
        header_info leg_header;
        leg_header.kind = header_kind::leg;
        leg_header.trip = trip;
        leg_header.leg = leg;
        leg_header.photos = static_cast<int>(leg.member_ids.size());
        headers_.push_back(std::move(leg_header));
        groups.push_back({images_of(leg.member_ids), true});
      }
    }
  }
  if (!review.unassigned_ids.empty()) {
    header_info unassigned;
    unassigned.kind = header_kind::unassigned;
    unassigned.photos = static_cast<int>(review.unassigned_ids.size());
    headers_.push_back(std::move(unassigned));
    groups.push_back({images_of(review.unassigned_ids), true});
  }

  auto screen = ui::column(8);
  screen->set_padding(basalt::length_insets{basalt::fixed(8), basalt::fixed(16), basalt::fixed(0),
                                            basalt::fixed(16)});
  screen->add_child(ui::text(summary_text(review), text_role::heading_small));
  toolbar_ = &screen->add_child(ui::row(8));
  settings_ = &screen->add_child(ui::column(4));
  build_toolbar();
  build_settings();

  if (review.trips.empty() && review.unassigned_ids.empty()) {
    auto empty = ui::centered(8);
    empty->add_child(ui::text("No trips found — photos may lack GPS or timestamp data"));
    ui::fill(*screen, std::move(empty));
    grid_.reset();
    return screen;
  }

  grid_ = std::make_unique<image_grid>(std::move(groups), loader_, cache_);
  grid_->set_header_factory([this](std::size_t section) { return make_header(section); });
  grid_->set_cell_decorator([this](basalt::image_tile &tile, image_info const &image) {
    tile.set_selected(state_.selecting && state_.selected.contains(image.id));
  });
  grid_->set_on_activate([this](image_info const &image) {
    if (state_.selecting) {
      toggle_selection(image);
    } else if (open_detail_) {
      open_detail_(image);
    }
  });
  auto &view = ui::fill(*screen, grid_->make_widget());
  view.set_scroll_offset(state_.scroll);
  return screen;
}

auto trips_screen::remember_scroll() -> void {
  if (grid_ && grid_->view() != nullptr) {
    state_.scroll = grid_->view()->scroll_offset();
  }
}

auto trips_screen::update() -> void {
  if (controls_dirty_) {
    controls_dirty_ = false;
    retired_.clear();
    build_toolbar();
    build_settings();
  }
  if (grid_) {
    if (headers_dirty_) {
      // The headers are rebuilt here and not in the handler of the button that asked for it.
      headers_dirty_ = false;
      grid_->reload();
    }
    grid_->apply_ready_images();
  }
}

auto trips_screen::toggle_selection(image_info const &image) -> void {
  if (!state_.selected.erase(image.id)) {
    state_.selected.insert(image.id);
  }
  controls_dirty_ = true;
  if (grid_) {
    grid_->reload();
  }
}

auto trips_screen::leave_selection() -> void {
  state_.selecting = false;
  state_.selected.clear();
  controls_dirty_ = true;
  if (grid_) {
    grid_->reload();
  }
}

auto trips_screen::build_toolbar() -> void {
  retire_children(*toolbar_);
  toolbar_->add_child(ui::make_button(
      state_.settings_open ? "Hide clustering settings" : "Clustering settings",
      [this] {
        state_.settings_open = !state_.settings_open;
        controls_dirty_ = true;
      },
      button_variant::tertiary));
  toolbar_->add_child(ui::make_button(
      state_.selecting ? "Cancel selection" : "Select photos",
      [this] {
        if (state_.selecting) {
          leave_selection();
        } else {
          state_.selecting = true;
          controls_dirty_ = true;
        }
      },
      button_variant::tertiary));
  if (state_.selecting && !state_.selected.empty()) {
    toolbar_->add_child(ui::text(std::format("{} selected", state_.selected.size())));
    auto &move = toolbar_->add_child(ui::make_button("Move to trip ▸", {}));
    move.set_on_press([this, &move] { open_move_menu(move); });
  }
}

auto trips_screen::build_settings() -> void {
  retire_children(*settings_);
  if (!state_.settings_open) {
    return;
  }
  auto const add_slider = [&](std::string label, float minimum, float maximum, char const *unit,
                              float &value) {
    auto &slider = settings_->add_child(
        std::make_unique<basalt::slider>(minimum, maximum, 1.0F, value));
    slider.set_label(std::move(label));
    slider.set_value_formatter(
        [unit](float shown) { return std::format("{} {}", static_cast<int>(shown), unit); });
    slider.set_on_change([&value](float changed) { value = changed; });
  };
  add_slider("Max time gap", 1, 168, "h", state_.gap_hours);
  add_slider("Max trip drift", 10, 1000, "km", state_.distance_km);
  add_slider("Away-from-home distance", 1, 100, "km", state_.home_radius_km);
  add_slider("New-leg distance", 1, 200, "km", state_.leg_radius_km);

  auto actions = ui::row(12);
  auto &folders = actions->add_child(std::make_unique<basalt::toggle>(
      "Organize output into trip folders", controller_.organize_into_folders()));
  folders.set_on_change([this](bool on) { controller_.set_organize_into_folders(on); });
  actions->add_child(ui::make_button(
      "Re-cluster",
      [this] {
        // The edits are defined against the old clustering, so they go.
        wizard_controller::trip_params params;
        params.gap_hours = static_cast<int>(state_.gap_hours);
        params.distance_km = static_cast<int>(state_.distance_km);
        params.home_radius_km = static_cast<int>(state_.home_radius_km);
        params.leg_radius_km = static_cast<int>(state_.leg_radius_km);
        state_.selected.clear();
        state_.selecting = false;
        controller_.rerun_trips_pass(params);
      },
      button_variant::primary));
  settings_->add_child(std::move(actions));
}

auto trips_screen::open_move_menu(basalt::widget &anchor) -> void {
  auto *const host = basalt::overlay_host::find(anchor);
  if (host == nullptr) {
    return;
  }
  auto choices = std::make_unique<basalt::menu>("Move to trip");
  choices->add_item("new", "New trip");
  choices->add_item("none", "Remove from trips");
  choices->add_separator();
  for (auto const &trip : trips_) {
    choices->add_item(std::format("trip:{}", trip.id),
                      std::format("{} · {}", trip_label(trip), iso_date(trip.start)));
  }
  auto *const raw = choices.get();
  auto const id = host->show_popup(std::move(choices), anchor.bounds_in_root());
  raw->set_on_activate([this, host, id](std::string const &chosen) {
    host->dismiss(id);
    std::vector<std::string> const ids{state_.selected.begin(), state_.selected.end()};
    if (chosen == "new") {
      controller_.create_trip_from_images(ids);
    } else if (chosen == "none") {
      controller_.move_images_to_trip(ids, std::nullopt);
    } else if (chosen.starts_with("trip:")) {
      controller_.move_images_to_trip(ids, std::stoll(chosen.substr(5)));
    }
    leave_selection();
  });
}

auto trips_screen::make_header(std::size_t section) -> std::unique_ptr<basalt::widget> {
  if (section >= headers_.size()) {
    return std::make_unique<basalt::box>();
  }
  auto const &info = headers_[section];
  auto bar = ui::row(8);
  bar->set_padding(basalt::length_insets::symmetric(basalt::fixed(8), basalt::fixed(4)));
  switch (info.kind) {
  case header_kind::folder: {
    bar->set_surface(basalt::box_surface{});
    if (state_.renaming_folder == info.folder_name) {
      auto &field = ui::fill(*bar, std::make_unique<basalt::text_field>());
      field.set_text(info.folder_name);
      field.select_all();
      auto const commit = [this, name = info.folder_name](std::string const &text) {
        state_.renaming_folder.reset();
        headers_dirty_ = true;
        if (!text.empty() && text != name) {
          // Every trip in the folder takes the new name.
          for (auto const &folder : folders_) {
            if (folder.name == name) {
              for (auto const &trip : folder.trips) {
                controller_.rename_trip_folder(trip.id, text);
              }
            }
          }
        }
      };
      field.set_on_submit(commit);
      field.set_on_focus_lost(commit);
      field.set_on_cancel([this] {
        state_.renaming_folder.reset();
        headers_dirty_ = true;
      });
    } else {
      ui::fill(*bar, ui::text(info.folder_name, text_role::body_strong));
      bar->add_child(ui::text(plural(static_cast<std::size_t>(info.folder_trips), "trip", "trips"),
                              text_role::small, text_tone::subdued));
      bar->add_child(ui::make_button(
          "Rename",
          [this, name = info.folder_name] {
            state_.renaming_folder = name;
            headers_dirty_ = true;
          },
          button_variant::tertiary));
    }
    break;
  }
  case header_kind::trip:
    ui::fill(*bar, ui::text(std::format("{}  ·  {} – {}", trip_label(info.trip),
                                        iso_date(info.trip.start), iso_date(info.trip.end)),
                            text_role::body));
    bar->add_child(ui::text(std::format("{} photos · {}", info.photos,
                                        format_duration(info.trip.start, info.trip.end)),
                            text_role::small, text_tone::subdued));
    break;
  case header_kind::leg:
    ui::fill(*bar, ui::text(info.leg.place_name.empty() ? std::format("Leg ({})", info.photos)
                                                         : info.leg.place_name,
                            text_role::small_strong, text_tone::subdued));
    break;
  case header_kind::unassigned:
    ui::fill(*bar, ui::text(std::format("Unassigned · {} photos", info.photos),
                            text_role::body_strong));
    bar->add_child(ui::text("Not in any trip (no GPS or timestamp, or removed)", text_role::small,
                            text_tone::subdued));
    break;
  }
  return bar;
}

} // namespace kustavi::gui
