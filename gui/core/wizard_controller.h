#pragma once

#include "core/background.h"
#include "core/domain.h"
#include "core/kustavi_client.h"
#include "core/batches.h"
#include "core/deletion.h"
#include "core/phases.h"
#include "core/trips_plan.h"
#include "core/ui_scope.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kustavi::gui {

// What the user can adjust and the back end persists. A resume restores them.
struct session_tunables {
  double blur_threshold = 100.0;
  double under_exposed_threshold = 0.3;
  double over_exposed_threshold = 0.3;
  int trip_gap_hours = 48;
  int trip_distance_km = 300;
  int trip_home_radius_km = 15;
  int trip_leg_radius_km = 25;

  [[nodiscard]] friend auto operator==(session_tunables const &, session_tunables const &)
      -> bool = default;
};

// The state machine behind the wizard screens. It runs on the UI thread, and so do all of its
// handlers. Results of the stream calls come back through the `ui_queue` and the unary calls run
// on the `background_executor`. Screens read `phase()` and rebuild when `on_change` fires.
//
// This part covers the start of the flow: choosing a folder, scanning it, resuming a saved
// session, confirming the result, and failing.
class wizard_controller {
public:
  wizard_controller(ui_queue &queue, background_executor executor);

  // Called after every change to the phase or the image index.
  auto on_change(std::function<void()> handler) -> void { on_change_ = std::move(handler); }

  // The back end's client. Null while the back end starts or restarts. Changing it does not
  // change the phase.
  auto set_client(std::shared_ptr<kustavi_client> client) -> void { client_ = std::move(client); }

  [[nodiscard]] auto phase() const -> wizard_phase const & { return phase_; }
  [[nodiscard]] auto source_folder() const -> std::string const & { return source_folder_; }
  [[nodiscard]] auto tunables() const -> session_tunables const & { return tunables_; }

  // The image index, in scan order.
  [[nodiscard]] auto image_ids() const -> std::vector<std::string> const & { return ordered_ids_; }
  [[nodiscard]] auto image(std::string const &id) const -> image_info const *;
  [[nodiscard]] auto image_count() const -> std::size_t { return ordered_ids_.size(); }

  // The stored pass results of a resumed session, kept for the screens that restore them.
  [[nodiscard]] auto restored_session() const -> GetSessionResultsResponse const * {
    return restored_.has_value() ? &*restored_ : nullptr;
  }

  // S0: the user chose a folder. A folder that holds a saved session leads to the restore prompt,
  // and any other folder to a scan.
  auto select_folder(std::string folder) -> void;
  // S0-B: continue from the saved session.
  auto resume_session() -> void;
  // S0-B: discard the saved session and scan the folder.
  auto start_fresh_from_restore() -> void;
  // S1: stop the scan and return to the start.
  auto cancel_scan() -> void;
  // S2: discard the scan and return to the start.
  auto back_from_confirm() -> void;
  // The error screen: return to where the failed call began.
  auto go_back_from_error() -> void;
  // S2: start the trips pass, whose folders the user reviews next.
  auto continue_from_confirm() -> void;
  // The trips pass is running: stop it and return to where it began.
  auto cancel_trips() -> void;
  // The trips review: return to the confirm screen.
  auto back_from_trips() -> void;

  // The trips review. Edits lay over the clustering and are saved with the session.
  struct trip_params {
    std::optional<int> gap_hours;
    std::optional<int> distance_km;
    std::optional<int> home_radius_km;
    std::optional<int> leg_radius_km;
  };
  // Runs the pass again with new settings. The edits are dropped, because they are defined against
  // the old clustering.
  auto rerun_trips_pass(trip_params params) -> void;
  // Moves photos to a trip, or out of every trip when `trip_id` is empty.
  auto move_images_to_trip(std::vector<std::string> const &image_ids,
                           std::optional<std::int64_t> trip_id) -> void;
  // Makes a trip from the photos and returns its id.
  auto create_trip_from_images(std::vector<std::string> const &image_ids) -> std::int64_t;
  // Renames the folder of a trip. An empty name is ignored.
  auto rename_trip_folder(std::int64_t trip_id, std::string name) -> void;
  auto set_organize_into_folders(bool organize) -> void;
  [[nodiscard]] auto organize_into_folders() const -> bool { return plan_.organize_into_folders(); }
  // The photos that some step marked for deletion. They leave their trips.
  auto set_deleted_ids(std::unordered_set<std::string> deleted) -> void;
  // The sub-path of each photo in the commit; empty when folders are off.
  [[nodiscard]] auto commit_folder_plan() const -> std::unordered_map<std::string, std::string>;
  // The time that a new trip is named from when no photo has a date.
  auto set_clock(std::function<time_ms()> clock) -> void { clock_ = std::move(clock); }

  // The trips review: the batches are the folders as they stand, and the batch menu opens.
  auto continue_from_trips() -> void;

  // ---- The batch menu and its passes ----

  [[nodiscard]] auto batches() const -> std::vector<batch_info> const & { return batches_; }
  [[nodiscard]] auto flags() const -> review_flags const & { return flags_; }
  [[nodiscard]] auto intent() const -> deletion_intent const & { return intent_; }
  // The photos of the batch that a pass runs on or a review shows; nothing outside a batch.
  [[nodiscard]] auto review_scope() const -> std::optional<std::unordered_set<std::string>>;
  // The duplicate groups with a member in the review scope.
  [[nodiscard]] auto review_similar_groups() const -> std::vector<similar_group_info>;
  // Whether a photo is marked for deletion as `step` sees it.
  [[nodiscard]] auto is_marked(std::string const &id, std::optional<deletion_step> step) const
      -> bool;
  // Why a photo is deleted, as labels.
  [[nodiscard]] auto reasons_for(std::string const &id) const -> std::vector<std::string>;
  // The photos marked for deletion by any step, in scan order.
  [[nodiscard]] auto deleted_ids() const -> std::vector<std::string>;

  auto select_batch(std::string const &key) -> void;
  // Runs `step` on the selected batch; its review opens when it ends.
  auto start_batch_pass(wizard_step step) -> void;
  // Runs `step` on every batch that has not finished it, one after another.
  auto start_pass_on_all_batches(wizard_step step) -> void;
  // Opens the review of a pass that the selected batch finished.
  auto review_batch_pass(wizard_step step) -> void;
  // Done on any review: back to the menu.
  auto close_batch_review() -> void;
  // The menu: back to the trips, which rebuilds the batches when the user continues.
  auto reopen_organize() -> void;
  // A running pass: stop it and return to the menu.
  auto cancel_pass() -> void;
  // The junk preparation: stop waiting for the model and return to the menu.
  auto cancel_junk_prep() -> void;
  // What the app does to stop the model download.
  auto set_cancel_model_download(std::function<void()> cancel) -> void {
    cancel_model_download_ = std::move(cancel);
  }
  // The app tells the controller when the vision model is ready. A junk pass that waits starts.
  auto set_model_ready(bool ready) -> void;

  // The quality thresholds. A change asks the back end how many photos it would flag.
  auto set_blur_threshold(double value) -> void;
  auto set_under_exposed_threshold(double value) -> void;
  auto set_over_exposed_threshold(double value) -> void;
  auto reset_thresholds() -> void;
  auto rerun_quality_pass() -> void;

  // The user's choices. Each is saved with the session.
  auto toggle_deletion(std::string const &id) -> void;
  auto keep_image(std::string const &id) -> void;
  auto mark_image(std::string const &id) -> void;
  auto set_group_keeper(std::uint32_t group_id, std::string keeper_id) -> void;
  // Every photo that `step` flagged in the review scope.
  auto keep_all_flagged(deletion_step step) -> void;
  auto mark_all_flagged(deletion_step step) -> void;

  // Return to the start with nothing kept. The next folder begins a new back-end session.
  auto reset_to_start() -> void;

private:
  auto set_phase(wizard_phase next) -> void;
  auto notify() -> void;
  auto fail(backend_error error) -> void;
  auto clear_results() -> void;
  auto begin_scan(bool resume) -> void;
  auto probe_folder(std::uint64_t generation) -> void;
  auto on_scan_event(ScanEvent const &event) -> void;
  auto on_scan_done() -> void;
  auto on_resume_scan_done() -> void;
  auto apply_session_results(GetSessionResultsResponse results) -> void;
  auto persist_progress() -> void;
  auto persist_trip_edits() -> void;
  [[nodiscard]] auto context() const -> trip_context;
  auto start_trips_pass() -> void;
  auto on_trips_event(TripsEvent const &event) -> void;
  auto on_trips_done() -> void;
  auto publish_trips_review() -> void;
  [[nodiscard]] auto trips_request() const -> RunTripsPassRequest;

  // Batches and passes (wizard_controller_batches.cpp).
  auto enter_batch_mode() -> void;
  [[nodiscard]] auto batch_menu_phase() const -> wizard_batch_menu;
  [[nodiscard]] auto find_batch(std::string const &key) const -> batch_info const *;
  [[nodiscard]] auto scope_ids() const -> std::vector<std::string>;
  [[nodiscard]] auto count_in_scope(std::vector<std::string> const &ids) const -> int;
  [[nodiscard]] auto scope_count() const -> int;
  auto return_to_batch_menu() -> void;
  auto begin_pass(wizard_step step) -> void;
  auto finish_batch_run(wizard_step step) -> void;
  [[nodiscard]] auto review_phase_for(wizard_step step) const -> wizard_phase;
  [[nodiscard]] auto quality_review_phase() const -> wizard_quality_review;
  [[nodiscard]] auto junk_review_phase() const -> wizard_junk_review;
  [[nodiscard]] auto similar_review_phase() const -> wizard_similar_review;
  [[nodiscard]] auto video_review_phase() const -> wizard_video_review;
  auto start_quality_pass() -> void;
  auto start_junk_pass() -> void;
  auto start_similar_pass() -> void;
  auto start_video_pass() -> void;
  auto on_quality_event(QualityEvent const &event) -> void;
  auto on_junk_event(JunkEvent const &event) -> void;
  auto on_similar_event(SimilarEvent const &event) -> void;
  auto on_video_event(VideoEvent const &event) -> void;
  auto save_last_run_thresholds() -> void;
  [[nodiscard]] auto thresholds_changed() const -> bool;
  auto refresh_quality_preview() -> void;
  auto republish_review() -> void;
  // Works out what each step flags and which photos are deleted, after the flags or the choices
  // changed.
  auto recompute_deleted() -> void;
  // The deletion state changed: save it, recompute what is deleted and update the screen.
  auto deletion_changed() -> void;
  auto persist_decisions() -> void;
  [[nodiscard]] auto now() const -> time_ms;
  [[nodiscard]] auto flagged_in_scope(deletion_step step) const -> std::vector<std::string>;

  // Handlers for a stream that run on the UI thread and belong to the current operation.
  template <class Event>
  auto make_handlers(std::function<void(Event const &)> on_event, std::function<void()> on_done)
      -> stream_handlers<Event> {
    auto const generation = scope_.next_generation();
    stream_handlers<Event> handlers;
    handlers.on_event = [this, generation, on_event = std::move(on_event)](Event const &event) {
      scope_.post(generation, [on_event, event] { on_event(event); });
    };
    handlers.on_done = [this, generation, on_done = std::move(on_done)](
                           std::optional<backend_error> error) {
      scope_.post(generation, [this, on_done, error = std::move(error)]() mutable {
        if (error) {
          fail(std::move(*error));
        } else {
          on_done();
        }
      });
    };
    return handlers;
  }

  ui_scope scope_;
  background_executor executor_;
  std::shared_ptr<kustavi_client> client_;
  std::function<void()> on_change_;

  wizard_phase phase_{wizard_start{}};
  std::optional<wizard_phase> return_phase_;
  std::string source_folder_;
  session_tunables tunables_;

  std::unordered_map<std::string, image_info> images_;
  std::vector<std::string> ordered_ids_;
  std::optional<ScanComplete> pending_complete_;
  std::optional<GetSessionResultsResponse> restored_;

  // True from a resume scan until its results are applied. Progress is not saved meanwhile.
  bool resuming_ = false;
  // Saved trip edits that wait for the trips pass of a resume. While set, edits are not saved, so
  // that they do not overwrite what is waiting.
  std::optional<std::string> pending_trip_edits_;
  // The step to land on after the trips pass of a resume. While set, progress is not saved.
  std::optional<int> resume_target_step_;
  trips_plan plan_;
  std::unordered_set<std::string> deleted_ids_;

  // What the passes found and what the user decided.
  review_flags flags_;
  int video_total_ = 0;
  deletion_intent intent_;
  flag_sets sets_;
  bool hydrating_decisions_ = false;

  // The batches, from the trip folders as they stand when the menu opens.
  bool batch_mode_ = false;
  std::vector<batch_info> batches_;
  std::optional<std::string> selected_batch_;
  // The batch that a running or open pass belongs to.
  std::optional<std::string> active_batch_;
  pass_progress progress_;
  std::vector<std::string> run_all_queue_;
  std::optional<wizard_step> run_all_step_;

  // The last quality run's thresholds, to tell whether the sliders moved.
  bool has_last_run_thresholds_ = false;
  session_tunables last_run_;
  std::optional<int> preview_flagged_;
  std::uint64_t preview_seq_ = 0;

  // The junk pass's timing profile. It starts at the first progress event that follows a real gap,
  // because a resume sends the photos that are already classified in a burst.
  std::optional<time_ms> junk_profile_start_;
  std::optional<std::uint32_t> junk_profile_base_done_;
  std::optional<time_ms> junk_last_event_;
  std::uint32_t junk_last_done_ = 0;
  bool model_ready_ = false;
  std::function<void()> cancel_model_download_;
  std::function<time_ms()> clock_;
  stream_subscription pass_;
};

} // namespace kustavi::gui
