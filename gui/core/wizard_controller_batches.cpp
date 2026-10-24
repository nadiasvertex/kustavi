#include "core/wizard_controller.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

#include <grpcpp/support/status.h>

namespace kustavi::gui {

namespace {

// A flag with a value this build does not know is a mismatch with the back end.
auto protocol_error(std::invalid_argument const &error) -> backend_error {
  return backend_rpc{grpc::StatusCode::INTERNAL, error.what()};
}

} // namespace

auto wizard_controller::now() const -> time_ms {
  return clock_ ? clock_()
                : std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
}

// ---- Batches ----

auto wizard_controller::find_batch(std::string const &key) const -> batch_info const * {
  auto const found = std::ranges::find(batches_, key, &batch_info::key);
  return found == batches_.end() ? nullptr : &*found;
}

auto wizard_controller::review_scope() const -> std::optional<std::unordered_set<std::string>> {
  if (!active_batch_) {
    return std::nullopt;
  }
  auto const *batch = find_batch(*active_batch_);
  if (batch == nullptr) {
    return std::nullopt;
  }
  return std::unordered_set<std::string>{batch->image_ids.begin(), batch->image_ids.end()};
}

auto wizard_controller::scope_ids() const -> std::vector<std::string> {
  if (active_batch_) {
    if (auto const *batch = find_batch(*active_batch_)) {
      return batch->image_ids;
    }
  }
  return {};
}

auto wizard_controller::count_in_scope(std::vector<std::string> const &ids) const -> int {
  auto const scope = review_scope();
  if (!scope) {
    return static_cast<int>(ids.size());
  }
  return static_cast<int>(std::ranges::count_if(
      ids, [&](std::string const &id) { return scope->contains(id); }));
}

auto wizard_controller::scope_count() const -> int {
  auto const scope = review_scope();
  return scope ? static_cast<int>(scope->size()) : static_cast<int>(images_.size());
}

auto wizard_controller::review_similar_groups() const -> std::vector<similar_group_info> {
  auto const scope = review_scope();
  if (!scope) {
    return flags_.similar_groups;
  }
  std::vector<similar_group_info> groups;
  for (auto const &group : flags_.similar_groups) {
    if (std::ranges::any_of(group.member_ids,
                            [&](std::string const &id) { return scope->contains(id); })) {
      groups.push_back(group);
    }
  }
  return groups;
}

auto wizard_controller::enter_batch_mode() -> void {
  batches_ = build_batches(plan_.folders(context()), ordered_ids_,
                           plan_.unassigned_image_ids(context()));
  batch_mode_ = true;
  active_batch_.reset();
  run_all_queue_.clear();
  run_all_step_.reset();
  selected_batch_ = batches_.empty() ? std::nullopt : std::optional{batches_.front().key};
}

auto wizard_controller::batch_menu_phase() const -> wizard_batch_menu {
  wizard_batch_menu menu;
  for (auto const &batch : batches_) {
    menu.batches.push_back(summarize_batch(batch, images_, flags_, progress_));
  }
  menu.selected_key = selected_batch_;
  menu.marked_count = static_cast<int>(deleted_ids_.size());
  menu.total_images = static_cast<int>(images_.size());
  return menu;
}

auto wizard_controller::continue_from_trips() -> void {
  if (!std::holds_alternative<wizard_trips_review>(phase_)) {
    return;
  }
  enter_batch_mode();
  set_phase(batch_menu_phase());
}

auto wizard_controller::reopen_organize() -> void {
  if (!std::holds_alternative<wizard_batch_menu>(phase_)) {
    return;
  }
  batch_mode_ = false;
  progress_ = {};
  active_batch_.reset();
  publish_trips_review();
}

auto wizard_controller::select_batch(std::string const &key) -> void {
  if (!std::holds_alternative<wizard_batch_menu>(phase_) || find_batch(key) == nullptr) {
    return;
  }
  selected_batch_ = key;
  set_phase(batch_menu_phase());
}

auto wizard_controller::return_to_batch_menu() -> void {
  active_batch_.reset();
  run_all_queue_.clear();
  run_all_step_.reset();
  if (batch_mode_) {
    set_phase(batch_menu_phase());
  } else {
    set_phase(return_phase_.value_or(wizard_start{}));
  }
}

auto wizard_controller::start_batch_pass(wizard_step step) -> void {
  if (!std::holds_alternative<wizard_batch_menu>(phase_) || !selected_batch_ || !client_) {
    return;
  }
  run_all_queue_.clear();
  run_all_step_.reset();
  active_batch_ = selected_batch_;
  begin_pass(step);
}

auto wizard_controller::start_pass_on_all_batches(wizard_step step) -> void {
  if (!std::holds_alternative<wizard_batch_menu>(phase_) || !client_) {
    return;
  }
  std::vector<std::string> pending;
  for (auto const &batch : batches_) {
    if (!progress_.done(step, batch.key)) {
      pending.push_back(batch.key);
    }
  }
  if (pending.empty()) {
    return;
  }
  run_all_step_ = step;
  run_all_queue_.assign(pending.begin() + 1, pending.end());
  active_batch_ = pending.front();
  begin_pass(step);
}

auto wizard_controller::review_batch_pass(wizard_step step) -> void {
  if (!std::holds_alternative<wizard_batch_menu>(phase_) || !selected_batch_) {
    return;
  }
  active_batch_ = selected_batch_;
  set_phase(review_phase_for(step));
}

auto wizard_controller::review_phase_for(wizard_step step) const -> wizard_phase {
  switch (step) {
  case wizard_step::quality:
    return quality_review_phase();
  case wizard_step::duplicates:
    return similar_review_phase();
  case wizard_step::junk:
    return junk_review_phase();
  default:
    return video_review_phase();
  }
}

auto wizard_controller::begin_pass(wizard_step step) -> void {
  switch (step) {
  case wizard_step::quality:
    start_quality_pass();
    break;
  case wizard_step::duplicates:
    start_similar_pass();
    break;
  case wizard_step::junk:
    // Photos that are already classified are not sent again, so the flags that exist stay.
    if (model_ready_) {
      start_junk_pass();
    } else {
      set_phase(wizard_junk_prep{});
    }
    break;
  case wizard_step::video:
    start_video_pass();
    break;
  case wizard_step::select:
  case wizard_step::trips:
  case wizard_step::copy:
    break;
  }
}

// A pass ended for the active batch: record it, then start the next queued batch, return to the
// menu at the end of a run on all batches, or open the pass's review.
auto wizard_controller::finish_batch_run(wizard_step step) -> void {
  if (active_batch_) {
    progress_.mark_done(step, *active_batch_);
  }
  if (run_all_step_ == step) {
    if (!run_all_queue_.empty()) {
      active_batch_ = run_all_queue_.front();
      run_all_queue_.erase(run_all_queue_.begin());
      begin_pass(step);
    } else {
      return_to_batch_menu();
    }
    return;
  }
  set_phase(review_phase_for(step));
}

auto wizard_controller::close_batch_review() -> void {
  if (std::holds_alternative<wizard_quality_review>(phase_) ||
      std::holds_alternative<wizard_similar_review>(phase_) ||
      std::holds_alternative<wizard_junk_review>(phase_) ||
      std::holds_alternative<wizard_video_review>(phase_)) {
    return_to_batch_menu();
  }
}

auto wizard_controller::cancel_pass() -> void {
  if (!std::holds_alternative<wizard_quality_running>(phase_) &&
      !std::holds_alternative<wizard_junk_running>(phase_) &&
      !std::holds_alternative<wizard_similar_running>(phase_) &&
      !std::holds_alternative<wizard_video_running>(phase_)) {
    return;
  }
  pass_.cancel();
  scope_.next_generation();
  return_to_batch_menu();
}

auto wizard_controller::cancel_junk_prep() -> void {
  if (!std::holds_alternative<wizard_junk_prep>(phase_)) {
    return;
  }
  if (cancel_model_download_) {
    cancel_model_download_();
  }
  return_to_batch_menu();
}

auto wizard_controller::set_model_ready(bool ready) -> void {
  model_ready_ = ready;
  if (ready && std::holds_alternative<wizard_junk_prep>(phase_) && client_) {
    start_junk_pass();
  }
}

// ---- Review phases ----

auto wizard_controller::quality_review_phase() const -> wizard_quality_review {
  std::vector<std::string> flagged;
  for (auto const &entry : flags_.quality) {
    flagged.push_back(entry.first);
  }
  return {count_in_scope(flagged), scope_count(), thresholds_changed(), preview_flagged_};
}

auto wizard_controller::junk_review_phase() const -> wizard_junk_review {
  std::vector<std::string> flagged;
  for (auto const &entry : flags_.junk) {
    flagged.push_back(entry.first);
  }
  return {count_in_scope(flagged), scope_count()};
}

auto wizard_controller::similar_review_phase() const -> wizard_similar_review {
  auto const groups = review_similar_groups();
  int marked = 0;
  for (auto const &group : groups) {
    for (auto const &id : group.member_ids) {
      if (is_marked(id, deletion_step::similar)) {
        ++marked;
      }
    }
  }
  return {static_cast<int>(groups.size()), marked};
}

auto wizard_controller::video_review_phase() const -> wizard_video_review {
  std::vector<std::string> flagged;
  for (auto const &entry : flags_.video) {
    flagged.push_back(entry.first);
  }
  int total = video_total_;
  if (auto const scope = review_scope()) {
    total = static_cast<int>(std::ranges::count_if(*scope, [&](std::string const &id) {
      auto const *image = this->image(id);
      return image != nullptr && image->is_video;
    }));
  }
  return {count_in_scope(flagged), total};
}

auto wizard_controller::republish_review() -> void {
  // Assigned without `set_phase`, which would save the position again on every slider step.
  if (std::holds_alternative<wizard_quality_review>(phase_)) {
    phase_ = quality_review_phase();
  } else if (std::holds_alternative<wizard_junk_review>(phase_)) {
    phase_ = junk_review_phase();
  } else if (std::holds_alternative<wizard_similar_review>(phase_)) {
    phase_ = similar_review_phase();
  } else if (std::holds_alternative<wizard_video_review>(phase_)) {
    phase_ = video_review_phase();
  } else if (std::holds_alternative<wizard_batch_menu>(phase_)) {
    phase_ = batch_menu_phase();
  } else if (std::holds_alternative<wizard_trips_review>(phase_)) {
    publish_trips_review();
    return;
  } else {
    return;
  }
  notify();
}

// ---- Quality ----

auto wizard_controller::save_last_run_thresholds() -> void {
  has_last_run_thresholds_ = true;
  last_run_ = tunables_;
}

auto wizard_controller::thresholds_changed() const -> bool {
  return has_last_run_thresholds_ &&
         (tunables_.blur_threshold != last_run_.blur_threshold ||
          tunables_.under_exposed_threshold != last_run_.under_exposed_threshold ||
          tunables_.over_exposed_threshold != last_run_.over_exposed_threshold);
}

// Runs the pass over the active batch with the current sliders. Its flags are recomputed, so the
// batch's old ones go first.
auto wizard_controller::start_quality_pass() -> void {
  auto const scope = review_scope();
  std::erase_if(flags_.quality, [&](auto const &entry) {
    return !scope || scope->contains(entry.first);
  });
  save_last_run_thresholds();
  set_phase(wizard_quality_running{});
  RunQualityPassRequest request;
  request.set_blur_threshold(tunables_.blur_threshold);
  request.set_underexposed_threshold(tunables_.under_exposed_threshold);
  request.set_overexposed_threshold(tunables_.over_exposed_threshold);
  for (auto const &id : scope_ids()) {
    request.add_scope_image_ids(id);
  }
  request.set_batch_key(active_batch_.value_or(""));
  pass_ = client_->run_quality_pass(
      std::move(request),
      make_handlers<QualityEvent>([this](QualityEvent const &event) { on_quality_event(event); },
                                  [this] {
                                    if (!std::holds_alternative<wizard_quality_running>(phase_)) {
                                      return;
                                    }
                                    save_last_run_thresholds();
                                    preview_flagged_.reset();
                                    recompute_deleted();
                                    finish_batch_run(wizard_step::quality);
                                  }));
}

auto wizard_controller::on_quality_event(QualityEvent const &event) -> void {
  auto *running = std::get_if<wizard_quality_running>(&phase_);
  if (running == nullptr) {
    return;
  }
  switch (event.event_case()) {
  case QualityEvent::kProgress:
    running->done = event.progress().done();
    running->total = event.progress().total();
    notify();
    break;
  case QualityEvent::kFlag:
    try {
      auto flag = quality_flag_info_from(event.flag());
      auto id = flag.image_id;
      flags_.quality.insert_or_assign(std::move(id), std::move(flag));
    } catch (std::invalid_argument const &error) {
      fail(protocol_error(error));
    }
    break;
  case QualityEvent::kComplete:
  case QualityEvent::EVENT_NOT_SET:
    break;
  }
}

auto wizard_controller::refresh_quality_preview() -> void {
  if (!client_) {
    return;
  }
  auto const seq = ++preview_seq_;
  PreviewQualityThresholdsRequest request;
  auto *thresholds = request.mutable_thresholds();
  thresholds->set_blur_threshold(tunables_.blur_threshold);
  thresholds->set_underexposed_threshold(tunables_.under_exposed_threshold);
  thresholds->set_overexposed_threshold(tunables_.over_exposed_threshold);
  for (auto const &id : scope_ids()) {
    request.add_scope_image_ids(id);
  }
  executor_([this, poster = scope_.get_poster(), generation = scope_.generation(), seq,
             client = client_, request = std::move(request)]() mutable {
    auto response = client->preview_quality_thresholds(std::move(request));
    // The preview is advisory. A failure leaves the last count in place.
    if (!response) {
      return;
    }
    poster.post(generation, [this, seq, flagged = static_cast<int>(response->flagged())] {
      // Only the newest request may publish, so a slow answer for an earlier slider position
      // cannot replace a later one.
      if (seq != preview_seq_) {
        return;
      }
      preview_flagged_ = flagged;
      republish_review();
    });
  });
}

auto wizard_controller::set_blur_threshold(double value) -> void {
  if (value == tunables_.blur_threshold) {
    return;
  }
  tunables_.blur_threshold = value;
  republish_review();
  refresh_quality_preview();
}

auto wizard_controller::set_under_exposed_threshold(double value) -> void {
  if (value == tunables_.under_exposed_threshold) {
    return;
  }
  tunables_.under_exposed_threshold = value;
  republish_review();
  refresh_quality_preview();
}

auto wizard_controller::set_over_exposed_threshold(double value) -> void {
  if (value == tunables_.over_exposed_threshold) {
    return;
  }
  tunables_.over_exposed_threshold = value;
  republish_review();
  refresh_quality_preview();
}

auto wizard_controller::reset_thresholds() -> void {
  session_tunables const defaults;
  if (tunables_.blur_threshold == defaults.blur_threshold &&
      tunables_.under_exposed_threshold == defaults.under_exposed_threshold &&
      tunables_.over_exposed_threshold == defaults.over_exposed_threshold) {
    return;
  }
  tunables_.blur_threshold = defaults.blur_threshold;
  tunables_.under_exposed_threshold = defaults.under_exposed_threshold;
  tunables_.over_exposed_threshold = defaults.over_exposed_threshold;
  republish_review();
  refresh_quality_preview();
}

auto wizard_controller::rerun_quality_pass() -> void {
  if (!std::holds_alternative<wizard_quality_review>(phase_) || !thresholds_changed() ||
      !client_) {
    return;
  }
  start_quality_pass();
}

// ---- Junk ----

auto wizard_controller::start_junk_pass() -> void {
  junk_profile_start_.reset();
  junk_profile_base_done_.reset();
  junk_last_event_.reset();
  junk_last_done_ = 0;
  set_phase(wizard_junk_running{});
  RunJunkPassRequest request;
  for (auto const &id : deleted_before_junk(intent_, ordered_ids_, flags_)) {
    request.add_skip_image_ids(id);
  }
  for (auto const &id : scope_ids()) {
    request.add_scope_image_ids(id);
  }
  request.set_batch_key(active_batch_.value_or(""));
  pass_ = client_->run_junk_pass(
      std::move(request),
      make_handlers<JunkEvent>([this](JunkEvent const &event) { on_junk_event(event); }, [this] {
        if (!std::holds_alternative<wizard_junk_running>(phase_)) {
          return;
        }
        recompute_deleted();
        finish_batch_run(wizard_step::junk);
      }));
}

auto wizard_controller::on_junk_event(JunkEvent const &event) -> void {
  auto *running = std::get_if<wizard_junk_running>(&phase_);
  if (running == nullptr) {
    return;
  }
  switch (event.event_case()) {
  case JunkEvent::kProgress: {
    auto const done = event.progress().done();
    auto const total = event.progress().total();
    auto const at = now();
    // The profile starts at the first event that follows a real gap, which means that the vision
    // model spent time on a photo.
    if (junk_last_event_ && at - *junk_last_event_ >= std::chrono::milliseconds{250}) {
      if (!junk_profile_start_) {
        junk_profile_start_ = junk_last_event_;
      }
      if (!junk_profile_base_done_) {
        junk_profile_base_done_ = junk_last_done_;
      }
    }
    junk_last_event_ = at;
    junk_last_done_ = done;

    running->done = done;
    running->total = total;
    running->seconds_per_image.reset();
    running->estimated_completion.reset();
    if (junk_profile_start_ && junk_profile_base_done_) {
      auto const measured = static_cast<double>(done) - static_cast<double>(*junk_profile_base_done_);
      auto const elapsed_ms =
          std::chrono::duration<double, std::milli>(at - *junk_profile_start_).count();
      if (measured > 0 && elapsed_ms > 0) {
        auto const seconds = elapsed_ms / 1000 / measured;
        running->seconds_per_image = seconds;
        auto const remaining = total > done ? static_cast<double>(total - done) : 0.0;
        running->estimated_completion =
            at + std::chrono::milliseconds{static_cast<std::int64_t>(seconds * remaining * 1000)};
      }
    }
    notify();
    break;
  }
  case JunkEvent::kFlag: {
    auto flag = junk_flag_info_from(event.flag());
    auto id = flag.image_id;
    flags_.junk.insert_or_assign(std::move(id), std::move(flag));
    break;
  }
  case JunkEvent::kComplete:
  case JunkEvent::EVENT_NOT_SET:
    break;
  }
}

// ---- Duplicates ----

auto wizard_controller::start_similar_pass() -> void {
  // Groups are computed again, so the batch's old ones go.
  auto const scope = review_scope();
  std::erase_if(flags_.similar_groups, [&](similar_group_info const &group) {
    return !scope || std::ranges::any_of(group.member_ids, [&](std::string const &id) {
      return scope->contains(id);
    });
  });
  set_phase(wizard_similar_running{});
  RunSimilarPassRequest request;
  for (auto const &id : deleted_before_similar(intent_, ordered_ids_, flags_)) {
    request.add_skip_image_ids(id);
  }
  for (auto const &id : scope_ids()) {
    request.add_scope_image_ids(id);
  }
  request.set_batch_key(active_batch_.value_or(""));
  pass_ = client_->run_similar_pass(
      std::move(request),
      make_handlers<SimilarEvent>([this](SimilarEvent const &event) { on_similar_event(event); },
                                  [this] {
                                    if (!std::holds_alternative<wizard_similar_running>(phase_)) {
                                      return;
                                    }
                                    recompute_deleted();
                                    finish_batch_run(wizard_step::duplicates);
                                  }));
}

auto wizard_controller::on_similar_event(SimilarEvent const &event) -> void {
  auto *running = std::get_if<wizard_similar_running>(&phase_);
  if (running == nullptr) {
    return;
  }
  switch (event.event_case()) {
  case SimilarEvent::kProgress:
    running->done = event.progress().done();
    running->total = event.progress().total();
    notify();
    break;
  case SimilarEvent::kGroup:
    flags_.similar_groups.push_back(similar_group_info_from(event.group()));
    break;
  case SimilarEvent::kComplete:
  case SimilarEvent::EVENT_NOT_SET:
    break;
  }
}

// ---- Video ----

auto wizard_controller::start_video_pass() -> void {
  set_phase(wizard_video_running{});
  RunVideoPassRequest request;
  for (auto const &id : deleted_before_video(intent_, ordered_ids_, flags_)) {
    request.add_skip_video_ids(id);
  }
  for (auto const &id : scope_ids()) {
    request.add_scope_image_ids(id);
  }
  request.set_batch_key(active_batch_.value_or(""));
  pass_ = client_->run_video_pass(
      std::move(request),
      make_handlers<VideoEvent>([this](VideoEvent const &event) { on_video_event(event); }, [this] {
        if (!std::holds_alternative<wizard_video_running>(phase_)) {
          return;
        }
        recompute_deleted();
        finish_batch_run(wizard_step::video);
      }));
}

auto wizard_controller::on_video_event(VideoEvent const &event) -> void {
  auto *running = std::get_if<wizard_video_running>(&phase_);
  if (running == nullptr) {
    return;
  }
  switch (event.event_case()) {
  case VideoEvent::kProgress:
    video_total_ = static_cast<int>(event.progress().total());
    running->done = event.progress().done();
    running->total = event.progress().total();
    notify();
    break;
  case VideoEvent::kFlag: {
    auto flag = video_flag_info_from(event.flag());
    auto id = flag.video_id;
    flags_.video.insert_or_assign(std::move(id), std::move(flag));
    break;
  }
  case VideoEvent::kComplete:
  case VideoEvent::EVENT_NOT_SET:
    break;
  }
}

// ---- Deletion ----

auto wizard_controller::recompute_deleted() -> void {
  sets_ = flag_sets_of(intent_, flags_);
  auto const deleted = deleted_image_ids(intent_, ordered_ids_, flags_);
  deleted_ids_ = {deleted.begin(), deleted.end()};
}

auto wizard_controller::is_marked(std::string const &id, std::optional<deletion_step> step) const
    -> bool {
  return is_marked_for_deletion(intent_, id, step, sets_);
}

auto wizard_controller::reasons_for(std::string const &id) const -> std::vector<std::string> {
  return deletion_reasons(id, intent_, flags_);
}

auto wizard_controller::deleted_ids() const -> std::vector<std::string> {
  return deleted_image_ids(intent_, ordered_ids_, flags_);
}

auto wizard_controller::persist_decisions() -> void {
  if (hydrating_decisions_ || !client_ || std::holds_alternative<wizard_start>(phase_) ||
      std::holds_alternative<wizard_scanning>(phase_) ||
      std::holds_alternative<wizard_session_restore>(phase_)) {
    return;
  }
  SaveSessionStateRequest request;
  request.set_replace_decisions(true);
  for (auto const &id : intent_.explicit_kept) {
    auto *entry = request.add_decisions();
    entry->set_image_id(id);
    entry->set_decision(KEEP);
  }
  for (auto const &id : intent_.explicit_deleted) {
    auto *entry = request.add_decisions();
    entry->set_image_id(id);
    entry->set_decision(DELETE);
  }
  for (auto const &[group, keeper] : intent_.group_keepers) {
    (*request.mutable_group_keepers())[group] = keeper;
  }
  executor_([client = client_, request = std::move(request)]() mutable {
    (void)client->save_session_state(std::move(request));
  });
}

auto wizard_controller::deletion_changed() -> void {
  recompute_deleted();
  persist_decisions();
  republish_review();
}

auto wizard_controller::toggle_deletion(std::string const &id) -> void {
  intent_ = intent_.with_toggle(id);
  deletion_changed();
}

auto wizard_controller::keep_image(std::string const &id) -> void {
  intent_ = intent_.with_kept(id);
  deletion_changed();
}

auto wizard_controller::mark_image(std::string const &id) -> void {
  intent_ = intent_.with_marked(id);
  deletion_changed();
}

auto wizard_controller::set_group_keeper(std::uint32_t group_id, std::string keeper_id) -> void {
  intent_ = intent_.with_keeper(group_id, std::move(keeper_id));
  deletion_changed();
}

auto wizard_controller::flagged_in_scope(deletion_step step) const -> std::vector<std::string> {
  std::vector<std::string> ids;
  auto const scope = review_scope();
  auto const add = [&](std::string const &id) {
    if (!scope || scope->contains(id)) {
      ids.push_back(id);
    }
  };
  switch (step) {
  case deletion_step::quality:
    for (auto const &entry : flags_.quality) {
      add(entry.first);
    }
    break;
  case deletion_step::junk:
    for (auto const &entry : flags_.junk) {
      add(entry.first);
    }
    break;
  case deletion_step::video:
    for (auto const &entry : flags_.video) {
      add(entry.first);
    }
    break;
  case deletion_step::similar:
    break;
  }
  return ids;
}

auto wizard_controller::keep_all_flagged(deletion_step step) -> void {
  intent_ = intent_.with_keep_all(flagged_in_scope(step));
  deletion_changed();
}

auto wizard_controller::mark_all_flagged(deletion_step step) -> void {
  intent_ = intent_.with_mark_all(flagged_in_scope(step));
  deletion_changed();
}

} // namespace kustavi::gui
