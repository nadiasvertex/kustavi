#include "core/wizard_controller.h"

#include <filesystem>

namespace kustavi::gui {

auto wizard_controller::marked_by_batch() const -> std::vector<marked_group> {
  return kustavi::gui::marked_by_batch(batches_, images_, deleted_ids());
}

auto wizard_controller::continue_from_batches() -> void {
  if (!std::holds_alternative<wizard_batch_menu>(phase_)) {
    return;
  }
  active_batch_.reset();
  set_phase(wizard_deletion_review{static_cast<int>(deleted_ids_.size())});
}

auto wizard_controller::back_from_deletion_review() -> void {
  if (!std::holds_alternative<wizard_deletion_review>(phase_)) {
    return;
  }
  set_phase(batch_menu_phase());
}

auto wizard_controller::continue_from_deletion_review() -> void {
  if (!std::holds_alternative<wizard_deletion_review>(phase_)) {
    return;
  }
  set_phase(commit_summary_phase());
  request_estimate_now();
}

auto wizard_controller::suggested_destination() const -> std::string {
  if (source_folder_.empty()) {
    return {};
  }
  auto folder = std::filesystem::path{source_folder_}.lexically_normal();
  // A trailing separator leaves an empty file name.
  if (!folder.has_filename()) {
    folder = folder.parent_path();
  }
  auto const name = folder.filename().string();
  if (name.empty()) {
    return {};
  }
  return (folder.parent_path() / (name + "-kept")).string();
}

auto wizard_controller::effective_destination() const -> std::string {
  return commit_destination_.empty() ? suggested_destination() : commit_destination_;
}

auto wizard_controller::commit_summary_phase() const -> wizard_commit_summary {
  auto const keep = keep_ids(ordered_ids_, deleted_ids());
  wizard_commit_summary summary;
  for (auto const &id : keep) {
    if (auto const *image = this->image(id)) {
      summary.keep_bytes += image->size_bytes;
    }
  }
  summary.keep_count = static_cast<int>(keep.size());
  summary.left_behind_count = static_cast<int>(images_.size()) - summary.keep_count;
  summary.destination = effective_destination();
  if (commit_estimate_for_ == summary.destination) {
    summary.estimate = commit_estimate_;
  }
  return summary;
}

auto wizard_controller::commit_request(std::string const &destination,
                                       std::vector<std::string> const &keep) const
    -> CommitRequest {
  CommitRequest request;
  request.set_destination(destination);
  for (auto const &id : keep) {
    request.add_keep_ids(id);
  }
  for (auto const &[id, folder] : commit_folder_plan()) {
    (*request.mutable_folder_for_id())[id] = folder;
  }
  // The destination is a library that may already hold some of these photos.
  request.set_merge_existing(true);
  return request;
}

auto wizard_controller::set_commit_destination(std::string value) -> void {
  if (!std::holds_alternative<wizard_commit_summary>(phase_) || value == commit_destination_) {
    return;
  }
  commit_destination_ = std::move(value);
  // Assigned without `set_phase`, which would save the position again for every key typed.
  phase_ = commit_summary_phase();
  notify();
  refresh_commit_estimate();
}

auto wizard_controller::refresh_commit_estimate() -> void {
  if (timers_ && estimate_timer_) {
    timers_->cancel(*estimate_timer_);
    estimate_timer_.reset();
  }
  if (!std::holds_alternative<wizard_commit_summary>(phase_) || effective_destination().empty()) {
    return;
  }
  if (timers_) {
    estimate_timer_ = timers_->after(std::chrono::milliseconds{400}, [this] {
      estimate_timer_.reset();
      request_estimate_now();
    });
  } else {
    request_estimate_now();
  }
}

auto wizard_controller::request_estimate_now() -> void {
  auto const destination = effective_destination();
  if (!std::holds_alternative<wizard_commit_summary>(phase_) || destination.empty() || !client_) {
    return;
  }
  auto request = commit_request(destination, keep_ids(ordered_ids_, deleted_ids()));
  executor_([this, poster = scope_.get_poster(), generation = scope_.generation(),
             client = client_, destination, request = std::move(request)]() mutable {
    auto response = client->estimate_commit(std::move(request));
    // An estimate that cannot be read leaves the copy disabled. The user can ask again by
    // editing the destination.
    if (!response) {
      return;
    }
    poster.post(generation, [this, destination, estimate = std::move(*response)]() mutable {
      if (!std::holds_alternative<wizard_commit_summary>(phase_) ||
          destination != effective_destination()) {
        return;
      }
      commit_estimate_ = std::move(estimate);
      commit_estimate_for_ = destination;
      phase_ = commit_summary_phase();
      notify();
    });
  });
}

auto wizard_controller::back_from_commit_summary() -> void {
  if (!std::holds_alternative<wizard_commit_summary>(phase_)) {
    return;
  }
  if (timers_ && estimate_timer_) {
    timers_->cancel(*estimate_timer_);
    estimate_timer_.reset();
  }
  if (batch_mode_) {
    set_phase(wizard_deletion_review{static_cast<int>(deleted_ids_.size())});
  } else {
    publish_trips_review();
  }
}

auto wizard_controller::start_commit() -> void {
  if (!std::holds_alternative<wizard_commit_summary>(phase_) || !client_) {
    return;
  }
  auto const destination = effective_destination();
  if (destination.empty()) {
    return;
  }
  if (timers_ && estimate_timer_) {
    timers_->cancel(*estimate_timer_);
    estimate_timer_.reset();
  }
  return_phase_ = phase_;
  committed_destination_ = destination;
  commit_keep_ids_ = keep_ids(ordered_ids_, deleted_ids());
  commit_total_bytes_ = 0;
  for (auto const &id : commit_keep_ids_) {
    if (auto const *image = this->image(id)) {
      commit_total_bytes_ += image->size_bytes;
    }
  }
  commit_copied_ = 0;
  commit_skipped_ = 0;
  commit_already_present_ = 0;
  commit_errors_.clear();
  wizard_committing committing;
  committing.total = static_cast<std::uint32_t>(commit_keep_ids_.size());
  committing.total_bytes = commit_total_bytes_;
  set_phase(std::move(committing));
  pass_ = client_->commit(
      commit_request(destination, commit_keep_ids_),
      make_handlers<CommitEvent>([this](CommitEvent const &event) { on_commit_event(event); },
                                 [this] { on_commit_done(); }));
}

auto wizard_controller::cancel_commit() -> void {
  if (!std::holds_alternative<wizard_committing>(phase_)) {
    return;
  }
  pass_.cancel();
  scope_.next_generation();
  // Files that were copied stay. A commit again skips them.
  set_phase(commit_summary_phase());
  request_estimate_now();
}

auto wizard_controller::on_commit_event(CommitEvent const &event) -> void {
  auto *committing = std::get_if<wizard_committing>(&phase_);
  if (committing == nullptr) {
    return;
  }
  switch (event.event_case()) {
  case CommitEvent::kProgress: {
    auto const done = event.progress().done();
    // The progress carries file counts only, so the bytes are the sizes of the first `done` photos.
    std::uint64_t bytes = 0;
    for (std::size_t i = 0; i < done && i < commit_keep_ids_.size(); ++i) {
      if (auto const *image = this->image(commit_keep_ids_[i])) {
        bytes += image->size_bytes;
      }
    }
    committing->done = done;
    committing->total = event.progress().total();
    committing->current_name = event.progress().current_name();
    committing->done_bytes = bytes;
    notify();
    break;
  }
  case CommitEvent::kComplete:
    commit_copied_ = event.complete().copied();
    commit_skipped_ = event.complete().skipped();
    commit_already_present_ = event.complete().already_present();
    commit_errors_.assign(event.complete().errors().begin(), event.complete().errors().end());
    break;
  case CommitEvent::EVENT_NOT_SET:
    break;
  }
}

auto wizard_controller::on_commit_done() -> void {
  if (!std::holds_alternative<wizard_committing>(phase_)) {
    return;
  }
  set_phase(wizard_done{commit_copied_, commit_skipped_, commit_already_present_,
                        committed_destination_, commit_errors_});
}

} // namespace kustavi::gui
