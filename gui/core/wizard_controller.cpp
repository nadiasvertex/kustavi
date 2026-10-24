#include "core/wizard_controller.h"

#include <grpcpp/support/status.h>

#include <stdexcept>

namespace kustavi::gui {

namespace {

// What the user sees when a folder is chosen before the back end is up.
auto still_starting() -> backend_error {
  return backend_rpc{grpc::StatusCode::UNAVAILABLE,
                     "Back end is still starting up. Please wait a moment."};
}

// Restores a tunable. The protocol zeroes a field that was never set, so zero keeps the default.
template <class T> auto restore(T &target, T saved) -> void {
  if (saved > 0) {
    target = saved;
  }
}

} // namespace

wizard_controller::wizard_controller(ui_queue &queue, background_executor executor)
    : scope_(queue), executor_(std::move(executor)) {}

auto wizard_controller::image(std::string const &id) const -> image_info const * {
  auto const found = images_.find(id);
  return found == images_.end() ? nullptr : &found->second;
}

auto wizard_controller::notify() -> void {
  if (on_change_) {
    on_change_();
  }
}

auto wizard_controller::set_phase(wizard_phase next) -> void {
  phase_ = std::move(next);
  notify();
  // The saved position is only updated when the wizard rests on a screen, and not while a resume
  // is putting the restored state back.
  auto const resting = std::holds_alternative<wizard_confirm_folder>(phase_) ||
                       std::holds_alternative<wizard_trips_review>(phase_) ||
                       std::holds_alternative<wizard_quality_review>(phase_) ||
                       std::holds_alternative<wizard_similar_review>(phase_) ||
                       std::holds_alternative<wizard_junk_review>(phase_) ||
                       std::holds_alternative<wizard_video_review>(phase_) ||
                       std::holds_alternative<wizard_batch_menu>(phase_);
  if (resting && !resuming_ && !resume_target_step_) {
    persist_progress();
  }
}

auto wizard_controller::fail(backend_error error) -> void {
  resuming_ = false;
  set_phase(wizard_failed{std::move(error), step_index_of(phase_)});
}

auto wizard_controller::clear_results() -> void {
  pass_.cancel();
  scope_.next_generation();
  images_.clear();
  ordered_ids_.clear();
  pending_complete_.reset();
  restored_.reset();
  plan_ = trips_plan{};
  flags_ = {};
  sets_ = {};
  intent_ = {};
  deleted_ids_.clear();
  video_total_ = 0;
  batch_mode_ = false;
  batches_.clear();
  selected_batch_.reset();
  active_batch_.reset();
  progress_ = {};
  run_all_queue_.clear();
  run_all_step_.reset();
  has_last_run_thresholds_ = false;
  preview_flagged_.reset();
  pending_trip_edits_.reset();
  resume_target_step_.reset();
  resuming_ = false;
}

auto wizard_controller::persist_progress() -> void {
  if (!client_) {
    return;
  }
  SaveSessionStateRequest request;
  request.set_step(static_cast<std::uint32_t>(step_index_of(phase_)));
  auto *quality = request.mutable_quality_thresholds();
  quality->set_blur_threshold(tunables_.blur_threshold);
  quality->set_underexposed_threshold(tunables_.under_exposed_threshold);
  quality->set_overexposed_threshold(tunables_.over_exposed_threshold);
  auto *trips = request.mutable_trip_params();
  trips->set_max_gap_hours(tunables_.trip_gap_hours);
  trips->set_max_distance_km(tunables_.trip_distance_km);
  trips->set_home_radius_km(tunables_.trip_home_radius_km);
  trips->set_leg_radius_km(tunables_.trip_leg_radius_km);
  // Saving is best effort. A failure must not disturb the wizard, and the next resting screen
  // tries again.
  executor_([client = client_, request = std::move(request)]() mutable {
    (void)client->save_session_state(std::move(request));
  });
}

auto wizard_controller::select_folder(std::string folder) -> void {
  if (!std::holds_alternative<wizard_start>(phase_)) {
    return;
  }
  clear_results();
  return_phase_.reset();
  source_folder_ = std::move(folder);
  if (!client_) {
    set_phase(wizard_failed{still_starting(), 0});
    return;
  }
  probe_folder(scope_.generation());
}

// Asks whether the folder holds a saved session before scanning. The phase stays on the start
// screen while the call runs.
auto wizard_controller::probe_folder(std::uint64_t generation) -> void {
  executor_([this, poster = scope_.get_poster(), generation, client = client_,
             folder = source_folder_] {
    // A call that fails counts as a folder without a session.
    auto probe = client->inspect_session(folder).value_or(InspectSessionResponse{});
    poster.post(generation, [this, probe = std::move(probe)] {
      if (!std::holds_alternative<wizard_start>(phase_)) {
        return;
      }
      if (probe.has_session() && probe.image_count() > 0) {
        set_phase(wizard_session_restore{source_folder_, probe.image_count(),
                                         static_cast<int>(probe.resume_step())});
      } else {
        begin_scan(false);
      }
    });
  });
}

auto wizard_controller::begin_scan(bool resume) -> void {
  auto const generation = scope_.next_generation();
  resuming_ = resume;
  set_phase(wizard_scanning{.folder = source_folder_});
  ScanFolderRequest request;
  request.set_folder(source_folder_);
  request.set_recursive(true);
  request.set_resume(resume);
  stream_handlers<ScanEvent> handlers;
  handlers.on_event = [this, generation](ScanEvent const &event) {
    scope_.post(generation, [this, event] { on_scan_event(event); });
  };
  handlers.on_done = [this, generation, resume](std::optional<backend_error> error) {
    scope_.post(generation, [this, resume, error = std::move(error)]() mutable {
      if (error) {
        fail(std::move(*error));
      } else if (resume) {
        on_resume_scan_done();
      } else {
        on_scan_done();
      }
    });
  };
  pass_ = client_->scan_folder(std::move(request), std::move(handlers));
}

auto wizard_controller::resume_session() -> void {
  auto const *restore_phase = std::get_if<wizard_session_restore>(&phase_);
  if (restore_phase == nullptr || !client_) {
    return;
  }
  auto folder = restore_phase->folder;
  clear_results();
  source_folder_ = std::move(folder);
  begin_scan(true);
}

auto wizard_controller::start_fresh_from_restore() -> void {
  auto const *restore_phase = std::get_if<wizard_session_restore>(&phase_);
  if (restore_phase == nullptr || !client_) {
    return;
  }
  auto folder = restore_phase->folder;
  clear_results();
  return_phase_.reset();
  source_folder_ = std::move(folder);
  begin_scan(false);
}

auto wizard_controller::on_scan_event(ScanEvent const &event) -> void {
  auto *scanning = std::get_if<wizard_scanning>(&phase_);
  if (scanning == nullptr) {
    return;
  }
  switch (event.event_case()) {
  case ScanEvent::kProgress:
    scanning->files_seen = event.progress().files_seen();
    scanning->images_found = event.progress().images_found();
    scanning->current_path = event.progress().current_path();
    break;
  case ScanEvent::kImage: {
    auto info = image_info_from(event.image());
    if (!images_.contains(info.id)) {
      ordered_ids_.push_back(info.id);
    }
    scanning->images_found = static_cast<std::uint32_t>(ordered_ids_.size());
    scanning->current_path = info.name;
    images_.insert_or_assign(info.id, std::move(info));
    break;
  }
  case ScanEvent::kComplete:
    pending_complete_ = event.complete();
    return;
  case ScanEvent::EVENT_NOT_SET:
    return;
  }
  notify();
}

auto wizard_controller::on_scan_done() -> void {
  auto const *scanning = std::get_if<wizard_scanning>(&phase_);
  if (scanning == nullptr || !pending_complete_) {
    return;
  }
  auto folder = scanning->folder;
  auto const complete = *std::exchange(pending_complete_, std::nullopt);
  if (complete.images() == 0) {
    set_phase(wizard_no_images{std::move(folder)});
    return;
  }
  // A saved session was found earlier, by `select_folder`, so this scan is a new one.
  set_phase(wizard_confirm_folder{
      std::move(folder), static_cast<std::uint32_t>(ordered_ids_.size()),
      {complete.errors().begin(), complete.errors().end()}});
}

auto wizard_controller::on_resume_scan_done() -> void {
  if (!std::holds_alternative<wizard_scanning>(phase_)) {
    return;
  }
  auto const complete = std::exchange(pending_complete_, std::nullopt);
  auto const confirm = [this] {
    resuming_ = false;
    set_phase(wizard_confirm_folder{source_folder_,
                                    static_cast<std::uint32_t>(ordered_ids_.size()),
                                    {}});
  };
  if (!complete || complete->images() == 0) {
    confirm();
    return;
  }
  auto const generation = scope_.generation();
  executor_([this, poster = scope_.get_poster(), generation, client = client_] {
    auto results = client->get_session_results();
    poster.post(generation, [this, results = std::move(results)]() mutable {
      if (!results) {
        fail(std::move(results.error()));
        return;
      }
      apply_session_results(std::move(*results));
    });
  });
}

// Puts back the settings of the saved session. The pass results stay in `restored_` for the
// screens that show them. A session that got past the select step re-runs the trips pass, which is
// cheap and keeps no result of its own, and lays the saved edits over it.
auto wizard_controller::apply_session_results(GetSessionResultsResponse results) -> void {
  auto const &quality = results.quality_thresholds();
  restore(tunables_.blur_threshold, quality.blur_threshold());
  restore(tunables_.under_exposed_threshold, quality.underexposed_threshold());
  restore(tunables_.over_exposed_threshold, quality.overexposed_threshold());
  auto const &trips = results.trip_params();
  restore(tunables_.trip_gap_hours, trips.max_gap_hours());
  restore(tunables_.trip_distance_km, trips.max_distance_km());
  restore(tunables_.trip_home_radius_km, trips.home_radius_km());
  restore(tunables_.trip_leg_radius_km, trips.leg_radius_km());
  auto const saved_step = static_cast<int>(results.resume_step());
  pending_trip_edits_ = results.trip_edits();
  // The pass results and the user's choices go back as they were. Nothing runs again except the
  // trips pass below, whose folders are the batches.
  try {
    for (auto const &flag : results.quality_flags()) {
      auto info = quality_flag_info_from(flag);
      auto id = info.image_id;
      flags_.quality.insert_or_assign(std::move(id), std::move(info));
    }
  } catch (std::invalid_argument const &error) {
    fail(backend_rpc{grpc::StatusCode::INTERNAL, error.what()});
    return;
  }
  for (auto const &group : results.similar_groups()) {
    flags_.similar_groups.push_back(similar_group_info_from(group));
  }
  for (auto const &flag : results.junk_flags()) {
    auto info = junk_flag_info_from(flag);
    auto id = info.image_id;
    flags_.junk.insert_or_assign(std::move(id), std::move(info));
  }
  for (auto const &flag : results.video_flags()) {
    auto info = video_flag_info_from(flag);
    auto id = info.video_id;
    flags_.video.insert_or_assign(std::move(id), std::move(info));
  }
  video_total_ = static_cast<int>(results.video_total());
  for (auto const &entry : results.decisions()) {
    if (entry.decision() == DELETE) {
      intent_.explicit_deleted.insert(entry.image_id());
    } else if (entry.decision() == KEEP) {
      intent_.explicit_kept.insert(entry.image_id());
    }
  }
  for (auto const &[group, keeper] : results.group_keepers()) {
    intent_.group_keepers[group] = keeper;
  }
  if (results.quality_done()) {
    progress_.session.insert(index_of(wizard_step::quality));
  }
  if (results.similar_done()) {
    progress_.session.insert(index_of(wizard_step::duplicates));
  }
  if (results.junk_done()) {
    progress_.session.insert(index_of(wizard_step::junk));
  }
  if (results.video_done()) {
    progress_.session.insert(index_of(wizard_step::video));
  }
  for (auto const &entry : results.completed_batch_passes()) {
    progress_.batches.insert(entry);
  }
  save_last_run_thresholds();
  recompute_deleted();
  restored_ = std::move(results);
  resuming_ = false;
  auto const confirm = wizard_confirm_folder{
      source_folder_, static_cast<std::uint32_t>(ordered_ids_.size()), {}};
  if (saved_step < index_of(wizard_step::quality)) {
    pending_trip_edits_.reset();
    set_phase(confirm);
    return;
  }
  resume_target_step_ = saved_step;
  return_phase_ = confirm;
  start_trips_pass();
}

auto wizard_controller::cancel_scan() -> void {
  if (!std::holds_alternative<wizard_scanning>(phase_)) {
    return;
  }
  pass_.cancel();
  scope_.next_generation();
  pending_complete_.reset();
  resuming_ = false;
  set_phase(wizard_start{});
}

auto wizard_controller::back_from_confirm() -> void {
  if (!std::holds_alternative<wizard_confirm_folder>(phase_)) {
    return;
  }
  clear_results();
  set_phase(wizard_start{});
}

auto wizard_controller::go_back_from_error() -> void {
  if (!std::holds_alternative<wizard_failed>(phase_)) {
    return;
  }
  if (batch_mode_ && !batches_.empty()) {
    // A pass in a batch failed. Every result so far stays, and the menu opens.
    pass_.cancel();
    scope_.next_generation();
    return_to_batch_menu();
    return;
  }
  clear_results();
  set_phase(return_phase_.value_or(wizard_start{}));
}

auto wizard_controller::reset_to_start() -> void {
  clear_results();
  return_phase_.reset();
  set_phase(wizard_start{});
}

} // namespace kustavi::gui
