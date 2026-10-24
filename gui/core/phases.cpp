#include "core/phases.h"

namespace kustavi::gui {

namespace {

struct step_visitor {
  auto operator()(wizard_failed const &failed) const -> int { return failed.step_index; }
  auto operator()(wizard_trips_running const &) const -> int { return index_of(wizard_step::trips); }
  auto operator()(wizard_trips_review const &) const -> int { return index_of(wizard_step::trips); }
  auto operator()(wizard_quality_running const &) const -> int {
    return index_of(wizard_step::quality);
  }
  auto operator()(wizard_quality_review const &) const -> int {
    return index_of(wizard_step::quality);
  }
  auto operator()(wizard_junk_prep const &) const -> int { return index_of(wizard_step::junk); }
  auto operator()(wizard_junk_running const &) const -> int { return index_of(wizard_step::junk); }
  auto operator()(wizard_junk_review const &) const -> int { return index_of(wizard_step::junk); }
  auto operator()(wizard_similar_running const &) const -> int {
    return index_of(wizard_step::duplicates);
  }
  auto operator()(wizard_similar_review const &) const -> int {
    return index_of(wizard_step::duplicates);
  }
  auto operator()(wizard_video_running const &) const -> int {
    return index_of(wizard_step::video);
  }
  auto operator()(wizard_video_review const &) const -> int { return index_of(wizard_step::video); }
  auto operator()(wizard_batch_menu const &) const -> int { return index_of(wizard_step::quality); }
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
