#include "core/wizard_controller.h"

#include <chrono>

namespace kustavi::gui {

auto wizard_controller::context() const -> trip_context {
  return {images_, ordered_ids_, deleted_ids_};
}

auto wizard_controller::trips_request() const -> RunTripsPassRequest {
  RunTripsPassRequest request;
  request.set_max_gap_hours(tunables_.trip_gap_hours);
  request.set_max_distance_km(tunables_.trip_distance_km);
  request.set_home_radius_km(tunables_.trip_home_radius_km);
  request.set_leg_radius_km(tunables_.trip_leg_radius_km);
  return request;
}

auto wizard_controller::continue_from_confirm() -> void {
  if (!std::holds_alternative<wizard_confirm_folder>(phase_) || !client_) {
    return;
  }
  return_phase_ = phase_;
  start_trips_pass();
}

// Runs the pass. The clustering and the edits start empty; a resume lays the saved edits over the
// result when the pass ends.
auto wizard_controller::start_trips_pass() -> void {
  plan_.set_clustering({});
  plan_.reset_edits();
  persist_trip_edits();
  auto const generation = scope_.next_generation();
  set_phase(wizard_trips_running{});
  stream_handlers<TripsEvent> handlers;
  handlers.on_event = [this, generation](TripsEvent const &event) {
    scope_.post(generation, [this, event] { on_trips_event(event); });
  };
  handlers.on_done = [this, generation](std::optional<backend_error> error) {
    scope_.post(generation, [this, error = std::move(error)]() mutable {
      if (error) {
        fail(std::move(*error));
      } else {
        on_trips_done();
      }
    });
  };
  pass_ = client_->run_trips_pass(trips_request(), std::move(handlers));
}

auto wizard_controller::on_trips_event(TripsEvent const &event) -> void {
  auto *running = std::get_if<wizard_trips_running>(&phase_);
  if (running == nullptr) {
    return;
  }
  switch (event.event_case()) {
  case TripsEvent::kProgress:
    running->done = event.progress().done();
    running->total = event.progress().total();
    notify();
    break;
  case TripsEvent::kTrip:
    plan_.add_clustered_trip(trip_info_from(event.trip()));
    break;
  case TripsEvent::kComplete:
  case TripsEvent::EVENT_NOT_SET:
    break;
  }
}

auto wizard_controller::on_trips_done() -> void {
  if (!std::holds_alternative<wizard_trips_running>(phase_)) {
    return;
  }
  auto const saved = std::exchange(pending_trip_edits_, std::nullopt);
  if (saved) {
    if (auto const edits = trip_edits::decode(*saved)) {
      plan_.apply_edits(*edits, images_);
    }
  }
  if (resume_target_step_) {
    // A resumed session lands where it was: the batch menu, whose batches are the folders that
    // were just rebuilt.
    auto const target = *resume_target_step_;
    resume_target_step_.reset();
    enter_batch_mode();
    recompute_deleted();
    if (target == index_of(wizard_step::copy)) {
      set_phase(commit_summary_phase());
      request_estimate_now();
    } else {
      set_phase(batch_menu_phase());
    }
    return;
  }
  publish_trips_review();
}

auto wizard_controller::publish_trips_review() -> void {
  wizard_trips_review review;
  review.trips = plan_.effective_trips(context());
  review.folders = plan_.folders(context());
  review.unassigned_ids = plan_.unassigned_image_ids(context());
  review.marked_count = plan_.marked_count(context());
  set_phase(std::move(review));
}

auto wizard_controller::cancel_trips() -> void {
  if (!std::holds_alternative<wizard_trips_running>(phase_)) {
    return;
  }
  pass_.cancel();
  scope_.next_generation();
  pending_trip_edits_.reset();
  resume_target_step_.reset();
  set_phase(return_phase_.value_or(wizard_start{}));
}

auto wizard_controller::back_from_trips() -> void {
  if (!std::holds_alternative<wizard_trips_review>(phase_)) {
    return;
  }
  set_phase(return_phase_.value_or(wizard_start{}));
}

auto wizard_controller::rerun_trips_pass(trip_params params) -> void {
  if (!std::holds_alternative<wizard_trips_review>(phase_) || !client_) {
    return;
  }
  tunables_.trip_gap_hours = params.gap_hours.value_or(tunables_.trip_gap_hours);
  tunables_.trip_distance_km = params.distance_km.value_or(tunables_.trip_distance_km);
  tunables_.trip_home_radius_km = params.home_radius_km.value_or(tunables_.trip_home_radius_km);
  tunables_.trip_leg_radius_km = params.leg_radius_km.value_or(tunables_.trip_leg_radius_km);
  start_trips_pass();
}

auto wizard_controller::persist_trip_edits() -> void {
  if (pending_trip_edits_ || !client_) {
    return;
  }
  SaveSessionStateRequest request;
  request.set_trip_edits(plan_.edits().encode());
  executor_([client = client_, request = std::move(request)]() mutable {
    (void)client->save_session_state(std::move(request));
  });
}

auto wizard_controller::move_images_to_trip(std::vector<std::string> const &image_ids,
                                            std::optional<std::int64_t> trip_id) -> void {
  plan_.move_images(image_ids, trip_id);
  persist_trip_edits();
  if (std::holds_alternative<wizard_trips_review>(phase_)) {
    publish_trips_review();
  }
}

auto wizard_controller::create_trip_from_images(std::vector<std::string> const &image_ids)
    -> std::int64_t {
  auto const now = clock_ ? clock_()
                          : std::chrono::floor<std::chrono::milliseconds>(
                                std::chrono::system_clock::now());
  auto const id = plan_.create_trip(image_ids, context(), now);
  persist_trip_edits();
  if (std::holds_alternative<wizard_trips_review>(phase_)) {
    publish_trips_review();
  }
  return id;
}

auto wizard_controller::rename_trip_folder(std::int64_t trip_id, std::string name) -> void {
  if (name.empty()) {
    return;
  }
  plan_.rename_folder(trip_id, std::move(name));
  persist_trip_edits();
  if (std::holds_alternative<wizard_trips_review>(phase_)) {
    publish_trips_review();
  }
}

auto wizard_controller::set_organize_into_folders(bool organize) -> void {
  plan_.set_organize_into_folders(organize);
  persist_trip_edits();
  if (std::holds_alternative<wizard_trips_review>(phase_)) {
    publish_trips_review();
  }
}

auto wizard_controller::set_deleted_ids(std::unordered_set<std::string> deleted) -> void {
  deleted_ids_ = std::move(deleted);
  if (std::holds_alternative<wizard_trips_review>(phase_)) {
    publish_trips_review();
  }
}

auto wizard_controller::commit_folder_plan() const
    -> std::unordered_map<std::string, std::string> {
  return plan_.commit_folder_plan(context());
}

} // namespace kustavi::gui
