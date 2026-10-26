#pragma once

#include "app/image_grid.h"
#include "core/deletion.h"
#include "core/phases.h"
#include "core/wizard_controller.h"

#include "basalt/basalt.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace kustavi::gui {

// What a review remembers while the screen is built again after each choice.
struct review_view_state {
  bool delete_expanded = true;
  bool thresholds_open = false;
  float keep_scroll = 0;
  float delete_scroll = 0;
};

// The review of the photos that a pass flagged, for quality, junk and video. Photos to keep fill the
// main area, and the photos still marked for deletion sit in a panel below. A click moves a photo
// from one to the other. Every flagged photo starts in the delete panel, so the work is to rescue
// the ones to keep. The quality review adds the threshold sliders.
class flagged_review_screen {
public:
  // `rebuild` asks for the screen to be built again, such as after the delete panel is folded.
  flagged_review_screen(wizard_controller &controller, deletion_step step,
                        review_view_state &state, image_loader &loader, decoded_image_cache &cache,
                        std::function<void()> rebuild);
  flagged_review_screen(flagged_review_screen const &) = delete;
  auto operator=(flagged_review_screen const &) -> flagged_review_screen & = delete;

  // What the button in the corner of a photo does.
  auto set_open_detail(std::function<void(image_info const &)> open) -> void {
    open_detail_ = std::move(open);
  }
  [[nodiscard]] auto build(std::string header) -> std::unique_ptr<basalt::widget>;
  // Gives finished images to the cells. Call it every frame.
  auto update() -> void;
  auto remember_scroll() -> void;
  // The sliders or the preview changed and nothing else did. Updates the threshold panel without
  // building it again, so a slider that is being dragged stays.
  auto refresh_thresholds(wizard_quality_review const &review) -> void;

  // The label of a quality, junk or video flag as a cell shows it, such as "Blurry" or "screenshot · 90%".
  [[nodiscard]] static auto video_reason_label(std::string const &reason) -> std::string;

private:
  auto build_grid(std::vector<image_info> images, bool in_delete_section)
      -> std::pair<std::unique_ptr<image_grid>, std::unique_ptr<basalt::widget>>;
  auto labels_for(image_info const &image) const -> std::vector<std::string>;
  auto build_threshold_panel() -> std::unique_ptr<basalt::widget>;
  auto threshold_summary() const -> std::string;
  auto preview_text() const -> std::string;

  wizard_controller &controller_;
  deletion_step step_;
  review_view_state &state_;
  image_loader &loader_;
  decoded_image_cache &cache_;
  std::function<void()> rebuild_;
  std::function<void(image_info const &)> open_detail_;
  std::unordered_map<std::string, std::vector<image_info>> neighbors_;
  std::unique_ptr<image_grid> keep_grid_;
  std::unique_ptr<image_grid> delete_grid_;
  basalt::collection_view *keep_view_ = nullptr;
  basalt::collection_view *delete_view_ = nullptr;
  // The parts of the threshold panel that change without a rebuild.
  basalt::label *summary_label_ = nullptr;
  basalt::label *preview_label_ = nullptr;
  basalt::button *rerun_header_ = nullptr;
  basalt::button *rerun_panel_ = nullptr;
};

// The review of near-duplicate groups. Each group is a section of one grid. The recommended or
// chosen keeper of a group is marked, and a click on a photo makes it the keeper.
class similar_review_screen {
public:
  // `scroll` keeps the position while the screen is built again.
  similar_review_screen(wizard_controller &controller, image_loader &loader,
                        decoded_image_cache &cache, float &scroll);
  similar_review_screen(similar_review_screen const &) = delete;
  auto operator=(similar_review_screen const &) -> similar_review_screen & = delete;

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
  std::function<void(image_info const &)> open_detail_;
  std::vector<similar_group_info> groups_;
  std::unique_ptr<image_grid> grid_;
  float &scroll_;
};

} // namespace kustavi::gui
