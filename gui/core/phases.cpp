#include "core/phases.h"

namespace kustavi::gui {

namespace {

struct step_visitor {
  auto operator()(wizard_failed const &failed) const -> int { return failed.step_index; }
  // Every phase of the select stage reports the first step.
  template <class Phase> auto operator()(Phase const &) const -> int {
    return index_of(wizard_step::select);
  }
};

} // namespace

auto step_index_of(wizard_phase const &phase) -> int { return std::visit(step_visitor{}, phase); }

auto stage_index_of(wizard_phase const &phase) -> int {
  return stage_index_of_step(step_index_of(phase));
}

} // namespace kustavi::gui
