#pragma once

#include "app/image_grid.h"
#include "core/model_prep.h"
#include "core/phases.h"
#include "core/wizard_controller.h"

#include "basalt/basalt.h"

#include <functional>
#include <memory>
#include <string>

namespace kustavi::gui {

// What the screens ask of the application beyond the wizard itself.
struct shell_services {
  std::string version;
  // Opens the system's folder chooser. The chosen folder goes to the controller.
  std::function<void()> pick_folder;
  // Begins an orderly exit.
  std::function<void()> exit_app;
  // Shows the output of the back-end process.
  std::function<void()> show_backend_log;
  // The vision model download's cancel and retry buttons.
  std::function<void()> cancel_download;
  std::function<void()> retry_download;
};

// The step indicator at the top: one entry per stage, with the finished ones marked and the current
// one highlighted. `stage_index` equal to `wizard_stage_count` marks every stage as finished.
[[nodiscard]] auto build_step_indicator(int stage_index) -> std::unique_ptr<basalt::widget>;

// S0. The model card shows only while a download is in progress.
[[nodiscard]] auto build_start_screen(model_prep_state const &model, shell_services const &services)
    -> std::unique_ptr<basalt::widget>;

// S1. `busy` receives the indeterminate bar, which the shell ticks every frame.
[[nodiscard]] auto build_scanning_screen(wizard_scanning const &phase, basalt::progress_bar *&busy)
    -> std::unique_ptr<basalt::widget>;

// S0-B.
[[nodiscard]] auto build_session_restore_screen(wizard_session_restore const &phase,
                                                wizard_controller &controller)
    -> std::unique_ptr<basalt::widget>;

// S2. The grid shows every scanned image, read only.
[[nodiscard]] auto build_confirm_screen(wizard_confirm_folder const &phase, image_grid &grid)
    -> std::unique_ptr<basalt::widget>;

[[nodiscard]] auto build_no_images_screen(wizard_no_images const &phase)
    -> std::unique_ptr<basalt::widget>;

// A call ended in an error.
[[nodiscard]] auto build_failed_screen(wizard_failed const &phase, shell_services const &services)
    -> std::unique_ptr<basalt::widget>;

// The buttons along the bottom for the phase, or null when it has none.
[[nodiscard]] auto build_action_bar(wizard_phase const &phase, wizard_controller &controller,
                                    shell_services const &services)
    -> std::unique_ptr<basalt::widget>;

// "Preparing vision model — 42% (1.2 / 2.9 GB)".
[[nodiscard]] auto model_card_caption(model_prep_downloading const &state) -> std::string;

} // namespace kustavi::gui
