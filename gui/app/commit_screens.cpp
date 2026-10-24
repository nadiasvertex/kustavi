#include "app/commit_screens.h"

#include "app/ui_kit.h"
#include "core/format.h"

#include <format>

namespace kustavi::gui {

namespace {

using basalt::text_role;
using basalt::text_tone;

auto plural(std::uint64_t count, char const *one, char const *many) -> std::string {
  return std::format("{} {}", format_int(static_cast<std::int64_t>(count)), count == 1 ? one : many);
}

auto badges_of(std::vector<std::string> const &labels) -> std::unique_ptr<basalt::widget> {
  if (labels.empty()) {
    return nullptr;
  }
  auto row = ui::row(2);
  for (auto const &label : labels) {
    row->add_child(std::make_unique<basalt::chip>(label, basalt::control_size::small));
  }
  return row;
}

} // namespace

// ---- Final review ----

deletion_review_screen::deletion_review_screen(wizard_controller &controller, image_loader &loader,
                                               decoded_image_cache &cache, float &scroll)
    : controller_(controller), loader_(loader), cache_(cache), scroll_(scroll) {}

auto deletion_review_screen::remember_scroll() -> void {
  if (grid_ && grid_->view() != nullptr) {
    scroll_ = grid_->view()->scroll_offset();
  }
}

auto deletion_review_screen::update() -> void {
  if (grid_) {
    grid_->apply_ready_images();
  }
}

auto deletion_review_screen::build() -> std::unique_ptr<basalt::widget> {
  groups_ = controller_.marked_by_batch();
  std::size_t total = 0;
  std::vector<image_group> sections;
  for (auto const &group : groups_) {
    total += group.images.size();
    sections.push_back({group.images, true});
  }

  auto screen = ui::column(8);
  screen->set_padding(basalt::length_insets{basalt::fixed(8), basalt::fixed(16), basalt::fixed(0),
                                            basalt::fixed(16)});
  ui::rigid(*screen, ui::text(total == 0 ? "Nothing is marked for deletion"
                                         : std::format("{} photos marked for deletion",
                                                       format_int(static_cast<std::int64_t>(total))),
                              text_role::heading_small));
  ui::rigid(*screen, ui::text("Click a photo to keep it. Nothing is deleted: kept photos are "
                              "copied to the destination and the rest stay in the source folder.",
                              text_role::small, text_tone::subdued));
  if (total == 0) {
    grid_.reset();
    ui::fill(*screen, std::make_unique<basalt::box>());
    return screen;
  }
  grid_ = std::make_unique<image_grid>(std::move(sections), loader_, cache_);
  grid_->set_header_factory([this](std::size_t section) -> std::unique_ptr<basalt::widget> {
    auto bar = ui::row(8);
    bar->set_padding(basalt::length_insets::symmetric(basalt::fixed(8), basalt::fixed(4)));
    if (section < groups_.size()) {
      bar->add_child(ui::text(std::format("{} · {}", groups_[section].title,
                                          format_int(static_cast<std::int64_t>(
                                              groups_[section].images.size()))),
                              text_role::body_strong));
    }
    return bar;
  });
  grid_->set_cell_decorator([this](basalt::image_tile &tile, image_info const &image) {
    tile.set_badge(basalt::tile_corner::bottom_left, badges_of(controller_.reasons_for(image.id)));
  });
  grid_->set_on_activate([this](image_info const &image) { controller_.keep_image(image.id); });
  if (open_detail_) {
    grid_->set_on_open(open_detail_);
  }
  auto &view = ui::fill(*screen, grid_->make_widget());
  view.set_scroll_offset(scroll_);
  return screen;
}

// ---- Commit summary ----

commit_summary_screen::commit_summary_screen(
    wizard_controller &controller,
    std::function<void(std::function<void(std::string)>)> choose_folder)
    : controller_(controller), choose_folder_(std::move(choose_folder)) {}

auto commit_summary_screen::estimate_text(std::optional<EstimateCommitResponse> const &estimate)
    -> std::string {
  if (!estimate) {
    return "Checking free space…";
  }
  auto const new_bytes = estimate->new_bytes();
  auto const saved = estimate->total_bytes() - new_bytes;
  std::vector<std::string> parts{std::format("Needs {}", format_bytes(new_bytes))};
  if (estimate->free_bytes_known()) {
    parts.push_back(std::format("{} free", format_bytes(estimate->free_bytes())));
  }
  if (estimate->already_present() > 0) {
    parts.push_back(std::format("{} already there ({} not copied)",
                                format_int(estimate->already_present()), format_bytes(saved)));
  }
  std::string joined;
  for (auto const &part : parts) {
    joined += joined.empty() ? part : " · " + part;
  }
  return estimate->fits() ? joined
                          : std::format("Not enough space: {}. Choose another folder.", joined);
}

auto commit_summary_screen::can_copy(wizard_commit_summary const &summary) -> bool {
  auto const blank = std::ranges::all_of(summary.destination, [](unsigned char c) {
    return std::isspace(c) != 0;
  });
  return !blank && summary.space_confirmed();
}

auto commit_summary_screen::build(wizard_commit_summary const &summary)
    -> std::unique_ptr<basalt::widget> {
  auto screen = ui::centered(8);
  auto content = ui::column(8);
  content->add_child(ui::text(std::format("Keep {} photos ({})", format_int(summary.keep_count),
                                          format_bytes(summary.keep_bytes)),
                              text_role::heading_small));
  content->add_child(ui::text(summary.left_behind_count == 1
                                  ? "1 photo will be left behind"
                                  : std::format("{} photos will be left behind",
                                                format_int(summary.left_behind_count)),
                              text_role::body, text_tone::subdued));

  auto destination = ui::row(8);
  field_ = &ui::fill(*destination, std::make_unique<basalt::text_field>());
  field_->set_placeholder("Destination");
  field_->set_text(summary.destination);
  field_->set_on_change([this](std::string const &text) { controller_.set_commit_destination(text); });
  destination->add_child(ui::make_button("Choose folder…", [this] {
    choose_folder_([this](std::string folder) {
      if (field_ != nullptr) {
        field_->set_text(folder);
      }
      controller_.set_commit_destination(std::move(folder));
    });
  }));
  content->add_child(std::move(destination));

  estimate_ = &content->add_child(ui::text(estimate_text(summary.estimate)));
  estimate_->set_tone(summary.estimate && !summary.estimate->fits() ? text_tone::danger
                                                                    : text_tone::normal);
  content->add_child(ui::text("The folder is created if it does not exist. If it already holds a "
                              "library, photos it has are skipped and new ones join its existing "
                              "folders. Your original files are never modified.",
                              text_role::small, text_tone::subdued));
  screen->add(std::move(content), basalt::box_item{.basis = basalt::fixed(560), .max = basalt::fixed(560)});
  return screen;
}

auto commit_summary_screen::refresh(wizard_commit_summary const &summary) -> void {
  if (estimate_ != nullptr) {
    estimate_->set_text(estimate_text(summary.estimate));
    estimate_->set_tone(summary.estimate && !summary.estimate->fits() ? text_tone::danger
                                                                      : text_tone::normal);
  }
}

// ---- Copying and done ----

auto build_committing_screen(wizard_committing const &phase) -> std::unique_ptr<basalt::widget> {
  auto screen = ui::centered(12);
  screen->add_child(ui::text("Copying photos", text_role::heading_small));
  screen->add_child(std::make_unique<basalt::progress_bar>(
      phase.total > 0 ? std::optional<float>{std::min(1.0F, static_cast<float>(phase.done) /
                                                              static_cast<float>(phase.total))}
                      : std::nullopt));
  screen->add_child(ui::text(std::format("{} / {} files ({} / {})", format_int(phase.done),
                                         format_int(phase.total), format_bytes(phase.done_bytes),
                                         format_bytes(phase.total_bytes))));
  if (!phase.current_name.empty()) {
    screen->add_child(ui::text(phase.current_name, text_role::source, text_tone::subdued));
  }
  return screen;
}

auto build_done_screen(wizard_done const &phase) -> std::unique_ptr<basalt::widget> {
  auto screen = ui::centered(10);
  screen->add_child(ui::text(std::format("Copied {} to {}",
                                         plural(phase.copied_count, "photo", "photos"),
                                         phase.destination),
                             text_role::heading_small, text_tone::success));
  if (phase.already_present_count > 0) {
    screen->add_child(ui::text(std::format(
        "{} already in the destination and not copied again.",
        plural(phase.already_present_count, "photo was", "photos were"))));
  }
  screen->add_child(ui::text("Your original files were not modified. You can safely delete the "
                             "source folder."));
  std::vector<std::string> issues;
  if (phase.skipped_count > 0) {
    issues.push_back(std::format("{} skipped (a different file of the same name already exists).",
                                 plural(phase.skipped_count, "file was", "files were")));
  }
  issues.insert(issues.end(), phase.errors.begin(), phase.errors.end());
  if (!issues.empty()) {
    screen->add_child(ui::text("Some files need your attention:", text_role::body_strong,
                               text_tone::danger));
    // The first lines that fit; a long list is in the back end's log.
    constexpr std::size_t shown = 12;
    for (std::size_t i = 0; i < issues.size() && i < shown; ++i) {
      screen->add_child(ui::text(issues[i], text_role::source, text_tone::subdued));
    }
    if (issues.size() > shown) {
      screen->add_child(ui::text(std::format("and {} more", issues.size() - shown),
                                 text_role::small, text_tone::subdued));
    }
  }
  return screen;
}

} // namespace kustavi::gui
