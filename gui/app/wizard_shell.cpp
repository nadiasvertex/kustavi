#include "app/wizard_shell.h"

#include "app/batch_menu_screen.h"
#include "app/ui_kit.h"
#include "core/format.h"

#include <algorithm>
#include <chrono>
#include <format>

namespace kustavi::gui {

wizard_shell::wizard_shell(basalt::widget_tree &tree, wizard_controller &controller,
                           model_status &model, image_loader &loader, shell_services services)
    : tree_(tree), controller_(controller), model_(model), loader_(loader),
      services_(std::move(services)) {
  host_ = &tree_.emplace_root<basalt::overlay_host>();
  auto frame = ui::column(0);
  frame->set_cross_alignment(basalt::alignment::stretch);
  frame_ = &host_->set_content(std::move(frame));
}

wizard_shell::~wizard_shell() {
  // The grids refer to widgets in the tree, and their cells refer to the grids.
  grid_.reset();
  trips_.reset();
  flagged_.reset();
  similar_.reset();
}

auto wizard_shell::mark_dirty() -> void { dirty_ = true; }

auto wizard_shell::phase_changed() -> void {
  auto const &phase = controller_.phase();
  auto const *review = std::get_if<wizard_quality_review>(&phase);
  if (!dirty_ && review != nullptr && flagged_ && phase.index() == built_phase_index_ &&
      controller_.revision() == built_revision_) {
    flagged_->refresh_thresholds(*review);
    return;
  }
  dirty_ = true;
}

auto wizard_shell::update() -> void {
  if (dirty_) {
    rebuild();
  }
  if (grid_) {
    grid_->apply_ready_images();
  }
  if (trips_) {
    trips_->update();
  }
  if (flagged_) {
    flagged_->update();
  }
  if (similar_) {
    similar_->update();
  }
  if (busy_ != nullptr) {
    busy_->tick(0.02F);
  }
}

auto wizard_shell::rebuild() -> void {
  dirty_ = false;
  // The grids go before the widgets that show them.
  if (trips_) {
    trips_->remember_scroll();
  }
  if (flagged_) {
    flagged_->remember_scroll();
  }
  if (similar_) {
    similar_->remember_scroll();
  }
  grid_.reset();
  trips_.reset();
  flagged_.reset();
  similar_.reset();
  auto const phase_index = controller_.phase().index();
  if (phase_index != built_phase_index_) {
    // A review of another kind starts at the top, with its panels as they were first.
    review_state_ = {};
    similar_scroll_ = 0;
  }
  built_phase_index_ = phase_index;
  built_revision_ = controller_.revision();
  auto const in_review = std::holds_alternative<wizard_trips_review>(controller_.phase());
  if (in_review && !was_trips_review_) {
    trips_state_ = {};
  }
  was_trips_review_ = in_review;
  busy_ = nullptr;
  frame_->clear_children();
  auto const &phase = controller_.phase();
  ui::rigid(*frame_, build_step_indicator(stage_index_of(phase)));
  ui::fill(*frame_, build_body());
  if (auto bar = build_action_bar(phase, controller_, services_)) {
    ui::rigid(*frame_, std::move(bar));
  }
}

auto wizard_shell::build_body() -> std::unique_ptr<basalt::widget> {
  auto const &phase = controller_.phase();
  struct visitor {
    wizard_shell &shell;

    auto operator()(wizard_start const &) const -> std::unique_ptr<basalt::widget> {
      return build_start_screen(shell.model_.state(), shell.services_);
    }
    auto operator()(wizard_scanning const &scanning) const -> std::unique_ptr<basalt::widget> {
      return build_scanning_screen(scanning, shell.busy_);
    }
    auto operator()(wizard_session_restore const &restore) const
        -> std::unique_ptr<basalt::widget> {
      return build_session_restore_screen(restore, shell.controller_);
    }
    auto operator()(wizard_confirm_folder const &confirm) const -> std::unique_ptr<basalt::widget> {
      std::vector<image_info> images;
      images.reserve(shell.controller_.image_count());
      for (auto const &id : shell.controller_.image_ids()) {
        images.push_back(*shell.controller_.image(id));
      }
      shell.grid_ = std::make_unique<image_grid>(std::move(images), shell.loader_, shell.images_);
      return build_confirm_screen(confirm, *shell.grid_);
    }
    auto operator()(wizard_no_images const &none) const -> std::unique_ptr<basalt::widget> {
      return build_no_images_screen(none);
    }
    auto operator()(wizard_trips_running const &running) const -> std::unique_ptr<basalt::widget> {
      return build_progress_screen("Grouping by trip", running.done, running.total, shell.busy_);
    }
    auto operator()(wizard_trips_review const &review) const -> std::unique_ptr<basalt::widget> {
      shell.trips_ = std::make_unique<trips_screen>(shell.controller_, shell.trips_state_,
                                                    shell.loader_, shell.images_);
      return shell.trips_->build(review);
    }
    auto operator()(wizard_quality_running const &running) const
        -> std::unique_ptr<basalt::widget> {
      return build_progress_screen("Checking image quality", running.done, running.total,
                                   shell.busy_);
    }
    auto operator()(wizard_quality_review const &review) const -> std::unique_ptr<basalt::widget> {
      return shell.flagged_review(deletion_step::quality,
                                  std::format("{} of {} images flagged", review.flagged_count,
                                              format_int(review.total_images)));
    }
    auto operator()(wizard_junk_prep const &) const -> std::unique_ptr<basalt::widget> {
      return build_junk_prep_screen(shell.model_.state(), shell.services_);
    }
    auto operator()(wizard_junk_running const &running) const -> std::unique_ptr<basalt::widget> {
      auto const at = std::chrono::floor<std::chrono::milliseconds>(
          std::chrono::system_clock::now());
      std::optional<civil_time> estimate;
      if (running.estimated_completion) {
        estimate = to_local_civil(*running.estimated_completion);
      }
      return build_progress_screen("Classifying images", running.done, running.total,
                                   shell.busy_,
                                   junk_timing_caption(running.seconds_per_image, estimate,
                                                       to_local_civil(at)));
    }
    auto operator()(wizard_junk_review const &review) const -> std::unique_ptr<basalt::widget> {
      return shell.flagged_review(deletion_step::junk,
                                  std::format("{} of {} images flagged", review.flagged_count,
                                              format_int(review.total_images)));
    }
    auto operator()(wizard_similar_running const &running) const
        -> std::unique_ptr<basalt::widget> {
      return build_progress_screen("Finding similar photos", running.done, running.total,
                                   shell.busy_);
    }
    auto operator()(wizard_similar_review const &) const -> std::unique_ptr<basalt::widget> {
      shell.similar_ = std::make_unique<similar_review_screen>(
          shell.controller_, shell.loader_, shell.images_, shell.similar_scroll_);
      return shell.similar_->build();
    }
    auto operator()(wizard_video_running const &running) const -> std::unique_ptr<basalt::widget> {
      return build_progress_screen("Checking videos", running.done, running.total, shell.busy_);
    }
    auto operator()(wizard_video_review const &review) const -> std::unique_ptr<basalt::widget> {
      return shell.flagged_review(deletion_step::video,
                                  std::format("{} of {} videos flagged", review.flagged_count,
                                              format_int(review.total_videos)));
    }
    auto operator()(wizard_batch_menu const &menu) const -> std::unique_ptr<basalt::widget> {
      return build_batch_menu_screen(menu, shell.controller_);
    }
    auto operator()(wizard_failed const &failed) const -> std::unique_ptr<basalt::widget> {
      return build_failed_screen(failed, shell.services_);
    }
  };
  return std::visit(visitor{*this}, phase);
}

auto wizard_shell::flagged_review(deletion_step step, std::string header)
    -> std::unique_ptr<basalt::widget> {
  flagged_ = std::make_unique<flagged_review_screen>(controller_, step, review_state_, loader_,
                                                     images_, [this] { mark_dirty(); });
  return flagged_->build(std::move(header));
}

auto wizard_shell::show_crash_dialog(std::string message, std::function<void()> on_retry)
    -> void {
  auto crash = std::make_unique<basalt::dialog>(
      "Processing error", std::move(message),
      std::vector<basalt::dialog_button>{{"Exit app", basalt::button_variant::secondary},
                                         {"Retry", basalt::button_variant::primary}});
  auto *const raw = crash.get();
  auto const id = host_->show_dialog(std::move(crash));
  raw->set_on_choice([this, id, on_retry = std::move(on_retry)](std::size_t choice) {
    host_->dismiss(id);
    if (choice == 1) {
      on_retry();
    } else {
      services_.exit_app();
    }
  });
}

auto wizard_shell::show_log_dialog(std::string const &log) -> void {
  // The dialog wraps a message, so a long log shows its last lines.
  constexpr std::size_t shown_lines = 30;
  std::vector<std::string_view> lines;
  for (std::size_t start = 0; start < log.size();) {
    auto const end = std::min(log.find('\n', start), log.size());
    lines.emplace_back(log.data() + start, end - start);
    start = end + 1;
  }
  std::string text;
  for (auto line = lines.size() > shown_lines ? lines.end() - shown_lines : lines.begin();
       line != lines.end(); ++line) {
    text.append(*line).push_back('\n');
  }
  if (text.empty()) {
    text = "The back end has not written anything yet.";
  }
  auto dialog = std::make_unique<basalt::dialog>(
      "Back-end log", std::move(text),
      std::vector<basalt::dialog_button>{{"Close", basalt::button_variant::primary}}, 0);
  auto *const raw = dialog.get();
  auto const id = host_->show_dialog(std::move(dialog));
  raw->set_on_choice([this, id](std::size_t) { host_->dismiss(id); });
}

} // namespace kustavi::gui
