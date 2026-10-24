#pragma once

#include "app/image_grid.h"
#include "app/image_loader.h"
#include "app/review_screen.h"
#include "app/screens.h"
#include "app/trips_screen.h"
#include "core/model_status.h"
#include "core/wizard_controller.h"

#include "basalt/basalt.h"

#include <functional>
#include <memory>

namespace kustavi::gui {

// The wizard's frame: the step indicator, the screen for the current phase, and the action bar. It
// builds the widget tree from the controller's state and builds it again when the state changes.
// The crash and log dialogs open over it.
class wizard_shell {
public:
  // `tree` gets the shell as its root. The controller, the model status and the loader outlive
  // the shell.
  wizard_shell(basalt::widget_tree &tree, wizard_controller &controller, model_status &model,
               image_loader &loader, shell_services services);
  wizard_shell(wizard_shell const &) = delete;
  auto operator=(wizard_shell const &) -> wizard_shell & = delete;
  ~wizard_shell();

  // The controller or the model status changed. The tree is rebuilt on the next `update`.
  auto mark_dirty() -> void;
  // The controller changed. A change that only moves the quality thresholds updates the panel in
  // place, so a slider that is being dragged stays. Any other change is built on the next `update`.
  auto phase_changed() -> void;
  // Rebuilds the screens if something changed, gives finished images to the grid, and moves the
  // busy bar. Call it once per frame, before layout.
  auto update() -> void;

  // The back end died or could not start. The dialog offers a retry or leaving.
  auto show_crash_dialog(std::string message, std::function<void()> on_retry) -> void;
  // The output of the back end, in a dialog.
  auto show_log_dialog(std::string const &log) -> void;

  [[nodiscard]] auto overlays() -> basalt::overlay_host & { return *host_; }
  [[nodiscard]] auto services() -> shell_services & { return services_; }
  // True while the screen needs another frame, such as a busy bar.
  [[nodiscard]] auto animating() const -> bool { return busy_ != nullptr; }

private:
  auto rebuild() -> void;
  auto build_body() -> std::unique_ptr<basalt::widget>;
  auto flagged_review(deletion_step step, std::string header) -> std::unique_ptr<basalt::widget>;

  basalt::widget_tree &tree_;
  wizard_controller &controller_;
  model_status &model_;
  image_loader &loader_;
  shell_services services_;
  basalt::overlay_host *host_ = nullptr;
  basalt::box *frame_ = nullptr;
  basalt::progress_bar *busy_ = nullptr;
  // Declared after the widgets that it points into: it goes first on destruction.
  decoded_image_cache images_;
  trips_view_state trips_state_;
  std::unique_ptr<trips_screen> trips_;
  review_view_state review_state_;
  std::unique_ptr<flagged_review_screen> flagged_;
  std::unique_ptr<similar_review_screen> similar_;
  float similar_scroll_ = 0;
  std::size_t built_phase_index_ = static_cast<std::size_t>(-1);
  std::uint64_t built_revision_ = 0;
  std::unique_ptr<image_grid> grid_;
  bool was_trips_review_ = false;
  bool dirty_ = true;
};

} // namespace kustavi::gui
