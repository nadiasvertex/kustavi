#include "app/review_screen.h"

#include "app/ui_kit.h"
#include "core/format.h"
#include "core/neighbors.h"

#include <algorithm>
#include <format>
#include <unordered_set>

namespace kustavi::gui {

namespace {

using basalt::button_variant;
using basalt::text_role;
using basalt::text_tone;

// A row of small pills that a cell shows over its picture.
auto make_badges(std::vector<std::string> const &labels) -> std::unique_ptr<basalt::widget> {
  if (labels.empty()) {
    return nullptr;
  }
  auto row = ui::row(2);
  for (auto const &label : labels) {
    row->add_child(std::make_unique<basalt::chip>(label, basalt::control_size::small));
  }
  return row;
}

auto empty_message(deletion_step step) -> std::string {
  switch (step) {
  case deletion_step::quality:
    return "No blurry or poorly exposed images found";
  case deletion_step::junk:
    return "No screenshots, scans, or memes found";
  case deletion_step::similar:
    return "No similar photos found";
  case deletion_step::video:
    return "No too-short, corrupt, blurry, or static videos found";
  }
  return {};
}

auto blur_text(double value) -> std::string {
  return value >= 100 && value == static_cast<int>(value) ? std::format("{}", static_cast<int>(value))
                                                          : std::format("{:.1f}", value);
}

} // namespace

// ---- Flagged review ----

flagged_review_screen::flagged_review_screen(wizard_controller &controller, deletion_step step,
                                             review_view_state &state, image_loader &loader,
                                             decoded_image_cache &cache,
                                             std::function<void()> rebuild)
    : controller_(controller), step_(step), state_(state), loader_(loader), cache_(cache),
      rebuild_(std::move(rebuild)) {}

auto flagged_review_screen::video_reason_label(std::string const &reason) -> std::string {
  auto spaced = reason;
  std::ranges::replace(spaced, '_', ' ');
  if (!spaced.empty()) {
    spaced[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(spaced[0])));
  }
  return spaced;
}

auto flagged_review_screen::labels_for(image_info const &image) const -> std::vector<std::string> {
  std::vector<std::string> labels;
  auto const &flags = controller_.flags();
  switch (step_) {
  case deletion_step::quality:
    if (auto const flag = flags.quality.find(image.id); flag != flags.quality.end()) {
      for (auto const reason : flag->second.reasons) {
        labels.emplace_back(label_of(reason));
      }
      if (auto const near = neighbors_.find(image.id); near != neighbors_.end()) {
        labels.push_back(std::format("{} nearby", near->second.size()));
      }
    }
    break;
  case deletion_step::junk:
    if (auto const flag = flags.junk.find(image.id); flag != flags.junk.end()) {
      labels.push_back(std::format("{} · {}%", flag->second.reason,
                                   static_cast<int>(std::lround(flag->second.confidence * 100))));
    }
    break;
  case deletion_step::video:
    if (auto const flag = flags.video.find(image.id); flag != flags.video.end()) {
      labels.push_back(std::format("{} · {}%", video_reason_label(flag->second.reason),
                                   static_cast<int>(std::lround(flag->second.confidence * 100))));
    }
    break;
  case deletion_step::similar:
    break;
  }
  return labels;
}

auto flagged_review_screen::build_grid(std::vector<image_info> images, bool in_delete_section)
    -> std::pair<std::unique_ptr<image_grid>, std::unique_ptr<basalt::widget>> {
  auto grid = std::make_unique<image_grid>(std::move(images), loader_, cache_);
  grid->set_cell_decorator([this](basalt::image_tile &tile, image_info const &image) {
    tile.set_badge(basalt::tile_corner::bottom_left, make_badges(labels_for(image)));
  });
  grid->set_on_activate([this, in_delete_section](image_info const &image) {
    if (in_delete_section) {
      controller_.keep_image(image.id);
    } else {
      controller_.mark_image(image.id);
    }
  });
  if (open_detail_) {
    grid->set_on_open(open_detail_);
  }
  auto widget = grid->make_widget();
  return {std::move(grid), std::move(widget)};
}

auto flagged_review_screen::build(std::string header) -> std::unique_ptr<basalt::widget> {
  // The photos of the batch that the pass flagged, in scan order. Junk and video go by confidence,
  // so that the clearest cuts are on top.
  auto const scope = controller_.review_scope();
  std::unordered_set<std::string> flagged;
  auto const &flags = controller_.flags();
  for (auto const &entry : flags.quality) {
    if (step_ == deletion_step::quality) {
      flagged.insert(entry.first);
    }
  }
  if (step_ == deletion_step::junk) {
    for (auto const &entry : flags.junk) {
      flagged.insert(entry.first);
    }
  } else if (step_ == deletion_step::video) {
    for (auto const &entry : flags.video) {
      flagged.insert(entry.first);
    }
  }
  std::vector<image_info> flagged_images;
  std::vector<image_info> candidates;
  for (auto const &id : controller_.image_ids()) {
    auto const *image = controller_.image(id);
    if (image == nullptr || (scope && !scope->contains(id))) {
      continue;
    }
    candidates.push_back(*image);
    if (flagged.contains(id)) {
      flagged_images.push_back(*image);
    }
  }
  auto const confidence_of = [&](image_info const &image) {
    if (step_ == deletion_step::junk) {
      return flags.junk.at(image.id).confidence;
    }
    return flags.video.at(image.id).confidence;
  };
  if (step_ == deletion_step::junk || step_ == deletion_step::video) {
    std::ranges::stable_sort(flagged_images, [&](auto const &a, auto const &b) {
      return confidence_of(a) > confidence_of(b);
    });
  }
  neighbors_.clear();
  if (step_ == deletion_step::quality) {
    std::unordered_set<std::string> quality_flagged;
    for (auto const &entry : flags.quality) {
      quality_flagged.insert(entry.first);
    }
    neighbors_ = find_neighbors(candidates, flagged, quality_flagged);
  }

  std::vector<image_info> keep_images;
  std::vector<image_info> delete_images;
  for (auto const &image : flagged_images) {
    (controller_.is_marked(image.id, step_) ? delete_images : keep_images).push_back(image);
  }

  auto screen = ui::column(0);
  screen->set_padding(basalt::length_insets{basalt::fixed(8), basalt::fixed(16), basalt::fixed(16),
                                            basalt::fixed(16)});
  auto top = ui::column(8);
  top->add_child(ui::text(std::move(header), text_role::heading_small));
  if (flagged_images.empty()) {
    ui::rigid(*screen, std::move(top));
    auto empty = ui::centered(8);
    empty->add_child(ui::text(empty_message(step_)));
    ui::fill(*screen, std::move(empty));
  } else {
    top->add_child(ui::text(std::format("Keeping {}", keep_images.size()),
                            text_role::body_strong));
    ui::rigid(*screen, std::move(top));
    keep_view_ = nullptr;
    delete_view_ = nullptr;
    auto const keep_empty = keep_images.empty();
    if (keep_empty) {
      ui::rigid(*screen, ui::text("Click a photo below to keep it", text_role::body,
                                  text_tone::subdued));
    } else {
      auto [grid, widget] = build_grid(std::move(keep_images), false);
      keep_grid_ = std::move(grid);
      keep_view_ = static_cast<basalt::collection_view *>(widget.get());
      screen->add(std::move(widget), basalt::box_item{.grow = 3, .basis = basalt::fixed(0)});
      keep_view_->set_scroll_offset(state_.keep_scroll);
    }
    ui::rigid(*screen, ui::make_button(
        std::format("{} Deleting {}", state_.delete_expanded ? "▾" : "▸", delete_images.size()),
        [this] {
          state_.delete_expanded = !state_.delete_expanded;
          rebuild_();
        },
        button_variant::tertiary));
    if (state_.delete_expanded) {
      if (delete_images.empty()) {
        ui::rigid(*screen, ui::text("Nothing marked for deletion", text_role::body,
                                    text_tone::subdued));
      } else {
        auto [grid, widget] = build_grid(std::move(delete_images), true);
        delete_grid_ = std::move(grid);
        delete_view_ = static_cast<basalt::collection_view *>(widget.get());
        screen->add(std::move(widget), basalt::box_item{.grow = keep_empty ? 3.0F : 2.0F,
                                                      .basis = basalt::fixed(0)});
        delete_view_->set_scroll_offset(state_.delete_scroll);
      }
    }
  }
  if (step_ == deletion_step::quality) {
    ui::rigid(*screen, build_threshold_panel());
  }
  return screen;
}

auto flagged_review_screen::remember_scroll() -> void {
  if (keep_view_ != nullptr && keep_grid_) {
    state_.keep_scroll = keep_view_->scroll_offset();
  }
  if (delete_view_ != nullptr && delete_grid_) {
    state_.delete_scroll = delete_view_->scroll_offset();
  }
}

auto flagged_review_screen::update() -> void {
  if (keep_grid_) {
    keep_grid_->apply_ready_images();
  }
  if (delete_grid_) {
    delete_grid_->apply_ready_images();
  }
}

// ---- Quality thresholds ----

auto flagged_review_screen::threshold_summary() const -> std::string {
  auto const &tunables = controller_.tunables();
  return std::format("Blur {} · Under {:.2f} · Over {:.2f}", blur_text(tunables.blur_threshold),
                     tunables.under_exposed_threshold, tunables.over_exposed_threshold);
}

auto flagged_review_screen::preview_text() const -> std::string {
  auto const *review = std::get_if<wizard_quality_review>(&controller_.phase());
  if (review == nullptr) {
    return {};
  }
  if (!review->preview_flagged) {
    return std::format("{} flagged at the last run", review->flagged_count);
  }
  return std::format("These settings flag {} of {} images ({} at the last run)",
                     *review->preview_flagged, format_int(review->total_images),
                     review->flagged_count);
}

auto flagged_review_screen::build_threshold_panel() -> std::unique_ptr<basalt::widget> {
  auto panel = ui::column(4);
  auto header = ui::row(8);
  header->add_child(ui::make_button(
      state_.thresholds_open ? "▾ Quality thresholds" : "▸ Quality thresholds",
      [this] {
        state_.thresholds_open = !state_.thresholds_open;
        rebuild_();
      },
      button_variant::tertiary));
  summary_label_ = &ui::fill(*header, ui::text(threshold_summary(), text_role::small,
                                                text_tone::subdued));
  summary_label_->set_visible(!state_.thresholds_open);
  rerun_header_ = &header->add_child(
      ui::make_button("Rerun pass", [this] { controller_.rerun_quality_pass(); }));
  panel->add_child(std::move(header));

  preview_label_ = nullptr;
  rerun_panel_ = nullptr;
  auto const *review = std::get_if<wizard_quality_review>(&controller_.phase());
  auto const rerun_enabled = review != nullptr && review->rerun_enabled;
  rerun_header_->set_visible(rerun_enabled);

  if (state_.thresholds_open) {
    auto const &tunables = controller_.tunables();
    auto sliders = ui::row(24);
    // Raising the blur threshold flags more images and raising an exposure threshold flags fewer,
    // so each slider says under its track which end flags more.
    auto const add_slider = [&](std::string label, float minimum, float maximum, float step,
                                double value, bool more_to_the_right,
                                std::function<std::string(float)> format,
                                std::function<void(double)> apply) {
      auto group = ui::column(2);
      auto &slider = group->add_child(std::make_unique<basalt::slider>(
          minimum, maximum, step, static_cast<float>(value)));
      slider.set_label(std::move(label));
      slider.set_value_formatter(std::move(format));
      slider.set_on_change([apply = std::move(apply)](float changed) {
        apply(static_cast<double>(changed));
      });
      auto ends = ui::row(8);
      ui::fill(*ends, ui::text(more_to_the_right ? "‹ Fewer flagged" : "‹ More flagged",
                               text_role::small, text_tone::subdued));
      ui::rigid(*ends, ui::text(more_to_the_right ? "More flagged ›" : "Fewer flagged ›",
                                text_role::small, text_tone::subdued));
      group->add_child(std::move(ends));
      ui::fill(*sliders, std::move(group));
    };
    add_slider("Blur threshold", 10, 500, 10, tunables.blur_threshold, true,
               [](float v) { return blur_text(v); },
               [this](double v) { controller_.set_blur_threshold(v); });
    add_slider("Underexposed", 0, 0.8F, 0.05F, tunables.under_exposed_threshold, false,
               [](float v) { return std::format("{:.2f}", v); },
               [this](double v) { controller_.set_under_exposed_threshold(v); });
    add_slider("Overexposed", 0, 0.8F, 0.05F, tunables.over_exposed_threshold, false,
               [](float v) { return std::format("{:.2f}", v); },
               [this](double v) { controller_.set_over_exposed_threshold(v); });
    panel->add_child(std::move(sliders));

    auto actions = ui::row(8);
    actions->add_child(ui::make_button("Reset to defaults",
                                       [this] { controller_.reset_thresholds(); },
                                       button_variant::tertiary));
    rerun_panel_ = &actions->add_child(ui::make_button(
        "Rerun pass", [this] { controller_.rerun_quality_pass(); }, button_variant::primary));
    rerun_panel_->set_visible(rerun_enabled);
    preview_label_ = &ui::fill(*actions, ui::text(preview_text(), text_role::body,
                                                   text_tone::subdued));
    panel->add_child(std::move(actions));
  }
  return panel;
}

auto flagged_review_screen::refresh_thresholds(wizard_quality_review const &review) -> void {
  if (summary_label_ != nullptr) {
    summary_label_->set_text(threshold_summary());
  }
  if (rerun_header_ != nullptr) {
    rerun_header_->set_visible(review.rerun_enabled);
  }
  if (rerun_panel_ != nullptr) {
    rerun_panel_->set_visible(review.rerun_enabled);
  }
  if (preview_label_ != nullptr) {
    preview_label_->set_text(preview_text());
  }
}

// ---- Duplicates ----

similar_review_screen::similar_review_screen(wizard_controller &controller, image_loader &loader,
                                             decoded_image_cache &cache, float &scroll)
    : controller_(controller), loader_(loader), cache_(cache), scroll_(scroll) {}

auto similar_review_screen::remember_scroll() -> void {
  if (grid_ && grid_->view() != nullptr) {
    scroll_ = grid_->view()->scroll_offset();
  }
}

auto similar_review_screen::update() -> void {
  if (grid_) {
    grid_->apply_ready_images();
  }
}

auto similar_review_screen::build() -> std::unique_ptr<basalt::widget> {
  groups_ = controller_.review_similar_groups();
  std::vector<image_group> sections;
  std::size_t total = 0;
  for (auto const &group : groups_) {
    image_group section;
    section.header = true;
    for (auto const &id : group.member_ids) {
      if (auto const *image = controller_.image(id)) {
        section.images.push_back(*image);
      }
    }
    total += group.member_ids.size();
    sections.push_back(std::move(section));
  }

  auto screen = ui::column(8);
  screen->set_padding(basalt::length_insets{basalt::fixed(8), basalt::fixed(16), basalt::fixed(0),
                                            basalt::fixed(16)});
  ui::rigid(*screen, ui::text(std::format("{} groups · {} images", groups_.size(), total),
                             text_role::heading_small));
  if (groups_.empty()) {
    auto empty = ui::centered(8);
    empty->add_child(ui::text("No similar photos found"));
    ui::fill(*screen, std::move(empty));
    grid_.reset();
    return screen;
  }

  // The group of each photo, to find its keeper.
  auto groups = std::make_shared<std::unordered_map<std::string, std::size_t>>();
  for (std::size_t index = 0; index < groups_.size(); ++index) {
    for (auto const &id : groups_[index].member_ids) {
      groups->emplace(id, index);
    }
  }
  grid_ = std::make_unique<image_grid>(std::move(sections), loader_, cache_);
  grid_->set_header_factory([this](std::size_t section) -> std::unique_ptr<basalt::widget> {
    auto bar = ui::row(8);
    bar->set_padding(basalt::length_insets::symmetric(basalt::fixed(8), basalt::fixed(4)));
    if (section < groups_.size()) {
      bar->add_child(ui::text(std::format("Group {} · {} photos", groups_[section].id + 1,
                                          groups_[section].member_ids.size()),
                              text_role::body_strong, text_tone::subdued));
    }
    return bar;
  });
  grid_->set_cell_decorator([this, groups](basalt::image_tile &tile, image_info const &image) {
    auto const found = groups->find(image.id);
    if (found == groups->end()) {
      return;
    }
    auto const &group = groups_[found->second];
    auto const chosen = controller_.intent().user_keeper_of(group.id);
    auto const keeper = chosen.value_or(group.recommended_keep_id);
    std::vector<std::string> labels;
    if (image.id == keeper) {
      labels.emplace_back(chosen ? "keeper" : "suggested");
    }
    if (controller_.is_marked(image.id, deletion_step::similar)) {
      labels.emplace_back("delete");
    }
    tile.set_selected(image.id == keeper);
    tile.set_badge(basalt::tile_corner::top_left, make_badges(labels));
  });
  if (open_detail_) {
    grid_->set_on_open(open_detail_);
  }
  grid_->set_on_activate([this, groups](image_info const &image) {
    if (auto const found = groups->find(image.id); found != groups->end()) {
      controller_.set_group_keeper(groups_[found->second].id, image.id);
    }
  });
  auto &view = ui::fill(*screen, grid_->make_widget());
  view.set_scroll_offset(scroll_);
  return screen;
}

} // namespace kustavi::gui
