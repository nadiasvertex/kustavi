#pragma once

#include "core/phases.h"
#include "core/wizard_controller.h"

#include "basalt/basalt.h"

#include <memory>
#include <string>

namespace kustavi::gui {

// The batch menu: the batches at the left, and at the right the passes of the selected batch, each
// with its state and the buttons to run it or open its review.
[[nodiscard]] auto build_batch_menu_screen(wizard_batch_menu const &menu,
                                           wizard_controller &controller)
    -> std::unique_ptr<basalt::widget>;

// "Done · 3 flagged", "Partly run · 1 flagged" or "Not run". Duplicates count groups.
[[nodiscard]] auto pass_summary(wizard_step step, batch_pass_status const &status) -> std::string;

} // namespace kustavi::gui
