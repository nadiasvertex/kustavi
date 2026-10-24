#pragma once

#include "app/image_grid.h"
#include "core/phases.h"
#include "core/wizard_controller.h"

#include "basalt/basalt.h"

#include <memory>
#include <string>

namespace kustavi::gui {

// The final review: every photo marked for deletion, grouped by batch, with the reasons. A click
// keeps a photo.
class deletion_review_screen {
public:
  // `scroll` keeps the position while the screen is built again.
  deletion_review_screen(wizard_controller &controller, image_loader &loader,
                         decoded_image_cache &cache, float &scroll);
  deletion_review_screen(deletion_review_screen const &) = delete;
  auto operator=(deletion_review_screen const &) -> deletion_review_screen & = delete;

  auto set_open_detail(std::function<void(image_info const &)> open) -> void {
    open_detail_ = std::move(open);
  }
  [[nodiscard]] auto build() -> std::unique_ptr<basalt::widget>;
  auto update() -> void;
  auto remember_scroll() -> void;

private:
  wizard_controller &controller_;
  image_loader &loader_;
  decoded_image_cache &cache_;
  float &scroll_;
  std::function<void(image_info const &)> open_detail_;
  std::vector<marked_group> groups_;
  std::unique_ptr<image_grid> grid_;
};

// The commit summary: the counts, the destination field and what the copy needs. The field and the
// estimate change while the user types, so the screen updates in place and keeps the field.
class commit_summary_screen {
public:
  // `choose_folder` opens the folder chooser and passes it the function to call with the choice.
  commit_summary_screen(wizard_controller &controller,
                        std::function<void(std::function<void(std::string)>)> choose_folder);
  commit_summary_screen(commit_summary_screen const &) = delete;
  auto operator=(commit_summary_screen const &) -> commit_summary_screen & = delete;

  [[nodiscard]] auto build(wizard_commit_summary const &summary) -> std::unique_ptr<basalt::widget>;
  // The estimate arrived or the destination changed.
  auto refresh(wizard_commit_summary const &summary) -> void;

  // "Needs 2.1 GB · 40 GB free" or "Not enough space: ...". Empty-handed text while it is unknown.
  [[nodiscard]] static auto estimate_text(std::optional<EstimateCommitResponse> const &estimate)
      -> std::string;
  // Whether Copy is enabled for the summary.
  [[nodiscard]] static auto can_copy(wizard_commit_summary const &summary) -> bool;

private:
  wizard_controller &controller_;
  std::function<void(std::function<void(std::string)>)> choose_folder_;
  basalt::text_field *field_ = nullptr;
  basalt::label *estimate_ = nullptr;
};

[[nodiscard]] auto build_committing_screen(wizard_committing const &phase)
    -> std::unique_ptr<basalt::widget>;
[[nodiscard]] auto build_done_screen(wizard_done const &phase) -> std::unique_ptr<basalt::widget>;

} // namespace kustavi::gui
