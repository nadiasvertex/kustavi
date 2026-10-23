#pragma once

#include "core/background.h"
#include "core/domain.h"
#include "core/kustavi_client.h"
#include "core/phases.h"
#include "core/ui_scope.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
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
  stream_subscription scan_;
};

} // namespace kustavi::gui
