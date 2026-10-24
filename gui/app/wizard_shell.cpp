#include "app/wizard_shell.h"

#include "app/batch_menu_screen.h"
#include "app/ui_kit.h"
#include "core/format.h"
#include "core/neighbors.h"

#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <format>

namespace kustavi::gui {

wizard_shell::wizard_shell(basalt::widget_tree &tree, wizard_controller &controller,
                           model_status &model, image_loader &loader, image_loader &master_loader,
                           shell_services services, video_player_factory videos)
    : tree_(tree), controller_(controller), model_(model), loader_(loader),
      master_loader_(master_loader), services_(std::move(services)), videos_(std::move(videos)) {
  host_ = &tree_.emplace_root<basalt::overlay_host>();
  auto frame = ui::column(0);
  frame->set_cross_alignment(basalt::alignment::stretch);
  frame_ = &host_->set_content(std::move(frame));
}

wizard_shell::~wizard_shell() {
  detail_.reset();
  retired_detail_.reset();
  // The grids refer to widgets in the tree, and their cells refer to the grids.
  grid_.reset();
  trips_.reset();
  flagged_.reset();
  similar_.reset();
  deletion_.reset();
  summary_.reset();
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
  auto const *summary = std::get_if<wizard_commit_summary>(&phase);
  if (!dirty_ && summary != nullptr && summary_ && phase.index() == built_phase_index_ &&
      controller_.revision() == built_revision_) {
    // The estimate or the destination changed. The field keeps its focus.
    summary_->refresh(*summary);
    if (copy_button_ != nullptr) {
      copy_button_->set_enabled(commit_summary_screen::can_copy(*summary));
    }
    return;
  }
  dirty_ = true;
}

auto wizard_shell::update() -> void {
  // A detail view that was closed goes now, after the handler that closed it has returned.
  retired_detail_.reset();
  if (dirty_) {
    rebuild();
  }
  if (detail_) {
    detail_->update();
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
  if (deletion_) {
    deletion_->update();
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
  if (deletion_) {
    deletion_->remember_scroll();
  }
  grid_.reset();
  trips_.reset();
  flagged_.reset();
  similar_.reset();
  deletion_.reset();
  summary_.reset();
  copy_button_ = nullptr;
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
  if (auto bar = build_action_bar(phase, controller_, services_, &copy_button_)) {
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
      shell.grid_->set_on_activate(
          [&shell = shell](image_info const &image) { shell.open_detail(image); });
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
      shell.trips_->set_open_detail(
          [&shell = shell](image_info const &image) { shell.open_detail(image); });
      shell.trips_->set_notify([&shell = shell](std::string const &text) { shell.show_toast(text); });
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
      shell.similar_->set_open_detail(
          [&shell = shell](image_info const &image) { shell.open_detail(image); });
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
    auto operator()(wizard_deletion_review const &) const -> std::unique_ptr<basalt::widget> {
      shell.deletion_ = std::make_unique<deletion_review_screen>(
          shell.controller_, shell.loader_, shell.images_, shell.review_state_.keep_scroll);
      shell.deletion_->set_open_detail(
          [&shell = shell](image_info const &image) { shell.open_detail(image); });
      return shell.deletion_->build();
    }
    auto operator()(wizard_commit_summary const &summary) const
        -> std::unique_ptr<basalt::widget> {
      shell.summary_ = std::make_unique<commit_summary_screen>(
          shell.controller_, shell.services_.choose_folder);
      return shell.summary_->build(summary);
    }
    auto operator()(wizard_committing const &committing) const
        -> std::unique_ptr<basalt::widget> {
      return build_committing_screen(committing);
    }
    auto operator()(wizard_done const &done) const -> std::unique_ptr<basalt::widget> {
      return build_done_screen(done);
    }
    auto operator()(wizard_failed const &failed) const -> std::unique_ptr<basalt::widget> {
      return build_failed_screen(failed);
    }
  };
  return std::visit(visitor{*this}, phase);
}

auto wizard_shell::flagged_review(deletion_step step, std::string header)
    -> std::unique_ptr<basalt::widget> {
  flagged_ = std::make_unique<flagged_review_screen>(controller_, step, review_state_, loader_,
                                                     images_, [this] { mark_dirty(); });
  flagged_->set_open_detail([this](image_info const &image) { open_detail(image); });
  return flagged_->build(std::move(header));
}

auto wizard_shell::detail_info_for(image_info const &image) const -> detail_info {
  detail_info info;
  info.image = image;
  auto const &phase = controller_.phase();
  // The confirm screen only looks. Everywhere else the switch works.
  info.can_toggle = !std::holds_alternative<wizard_confirm_folder>(phase);
  if (std::holds_alternative<wizard_quality_review>(phase)) {
    info.step = deletion_step::quality;
  } else if (std::holds_alternative<wizard_junk_review>(phase)) {
    info.step = deletion_step::junk;
  } else if (std::holds_alternative<wizard_video_review>(phase)) {
    info.step = deletion_step::video;
  } else if (std::holds_alternative<wizard_similar_review>(phase)) {
    info.step = deletion_step::similar;
  }
  auto const &flags = controller_.flags();
  if (auto const flag = flags.quality.find(image.id); flag != flags.quality.end()) {
    info.sharpness = flag->second.sharpness;
    info.exposure_score = flag->second.exposure_score;
  }
  if (auto const flag = flags.junk.find(image.id); flag != flags.junk.end()) {
    info.junk_reason = flag->second.reason;
    info.junk_confidence = flag->second.confidence;
  }
  if (auto const flag = flags.video.find(image.id); flag != flags.video.end()) {
    info.video_reason = flag->second.reason;
    info.video_confidence = flag->second.confidence;
  }
  // Where the photo sits in the trips.
  if (auto const *review = std::get_if<wizard_trips_review>(&phase)) {
    auto const iso = [](time_ms instant) {
      auto const civil = to_local_civil(instant);
      return std::format("{:04}-{:02}-{:02}", civil.year, civil.month, civil.day);
    };
    for (auto const &trip : review->trips) {
      auto const leg = std::ranges::find_if(trip.legs, [&](trip_leg_info const &candidate) {
        return std::ranges::find(candidate.member_ids, image.id) != candidate.member_ids.end();
      });
      if (std::ranges::find(trip.member_ids, image.id) == trip.member_ids.end()) {
        continue;
      }
      auto const label = trip.is_home ? std::string{"Home"}
                         : !trip.place_name.empty() ? trip.place_name
                                                    : trip.folder.value_or("Trip");
      info.mappings.emplace_back("Trip", std::format("{} · {} – {}", label, iso(trip.start),
                                                     iso(trip.end)));
      if (trip.legs.size() > 1 && leg != trip.legs.end() && !leg->place_name.empty()) {
        info.mappings.emplace_back("Leg", leg->place_name);
      }
      if (!trip.place_name.empty()) {
        info.mappings.emplace_back("Place", trip.place_name);
      }
      break;
    }
  }
  // The unflagged shots around a flagged photo, so the reviewer can see a better one.
  if (info.step == deletion_step::quality && flags.quality.contains(image.id)) {
    auto const scope = controller_.review_scope();
    std::vector<image_info> candidates;
    for (auto const &id : controller_.image_ids()) {
      if (auto const *candidate = controller_.image(id);
          candidate != nullptr && (!scope || scope->contains(id))) {
        candidates.push_back(*candidate);
      }
    }
    std::unordered_set<std::string> quality_flagged;
    for (auto const &entry : flags.quality) {
      quality_flagged.insert(entry.first);
    }
    auto const near = find_neighbors(candidates, {image.id}, quality_flagged);
    if (auto const found = near.find(image.id); found != near.end()) {
      info.neighbors = found->second;
    }
  }
  return info;
}

auto wizard_shell::open_detail(image_info const &image) -> void {
  close_detail();
  detail_ = std::make_unique<detail_view>(
      controller_, detail_info_for(image), master_loader_, videos_, services_.wake,
      detail_view::callbacks{
          [this] { close_detail(); },
          [this](image_info const &neighbor) { open_detail(neighbor); }},
      tree_.viewport());
  detail_overlay_ = host_->show_dialog(detail_->build(), [this] {
    // The overlay is gone, by Escape or because the owner dismissed it.
    detail_overlay_.reset();
    retired_detail_ = std::move(detail_);
  });
}

auto wizard_shell::show_toast(std::string const &text) -> void {
  if (toast_) {
    host_->dismiss(*toast_);
    toast_.reset();
  }
  if (timers_ && toast_timer_) {
    timers_->cancel(*toast_timer_);
    toast_timer_.reset();
  }
  auto message = ui::row(0);
  message->set_padding(basalt::length_insets::symmetric(basalt::fixed(16), basalt::fixed(10)));
  message->set_surface(basalt::box_surface{});
  message->add_child(ui::text(text));
  auto const view = tree_.viewport();
  // The popup goes below the anchor, or above when it does not fit, so the anchor sits at the
  // bottom edge, in the middle.
  basalt::rect const anchor{{view.width / 2, view.height - 72}, {1, 1}};
  toast_ = host_->show_hover_popup(std::move(message), anchor, [this] { toast_.reset(); });
  if (timers_) {
    toast_timer_ = timers_->after(std::chrono::seconds{4}, [this] {
      toast_timer_.reset();
      if (toast_) {
        host_->dismiss(*toast_);
      }
    });
  }
}

auto wizard_shell::close_detail() -> void {
  if (detail_overlay_) {
    // The close handler above retires the view.
    host_->dismiss(*detail_overlay_);
  }
}

} // namespace kustavi::gui
