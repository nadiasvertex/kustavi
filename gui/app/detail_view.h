#pragma once

#include "app/image_grid.h"
#include "app/image_loader.h"
#include "app/video_player.h"
#include "app/zoom_view.h"
#include "core/deletion.h"
#include "core/wizard_controller.h"

#include "basalt/basalt.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kustavi::gui {

// What the detail view shows about one photo besides the picture.
struct detail_info {
  image_info image;
  // The switch for deletion is enabled. It is not on the confirm screen, which only looks.
  bool can_toggle = false;
  // The step whose default decides whether the photo counts as marked.
  std::optional<deletion_step> step;
  std::optional<double> sharpness;
  std::optional<double> exposure_score;
  std::optional<std::string> junk_reason;
  std::optional<double> junk_confidence;
  std::optional<std::string> video_reason;
  std::optional<double> video_confidence;
  // Extra rows, such as the trip, leg and place.
  std::vector<std::pair<std::string, std::string>> mappings;
  // Unflagged photos taken just before and after.
  std::vector<image_info> neighbors;
};

// The detail of one photo: its metadata beside a picture that zooms and pans. The working preview
// shows at once, the full-size master replaces it when it has been decoded, and a video plays in
// place of the picture. It is a widget tree for an overlay, and the owner calls `update` every
// frame.
class detail_view {
public:
  struct callbacks {
    std::function<void()> close;
    // The user chose a neighbor. The owner closes this view and opens the neighbor.
    std::function<void(image_info const &)> open_neighbor;
  };

  // The view decodes the working preview and the master with `master_loader`, which no other screen
  // reads from, so nothing else takes its results. `size` is the room it may use.
  detail_view(wizard_controller &controller, detail_info info, image_loader &master_loader,
              video_player_factory videos, std::function<void()> wake, callbacks handlers,
              basalt::size size);
  detail_view(detail_view const &) = delete;
  auto operator=(detail_view const &) -> detail_view & = delete;

  [[nodiscard]] auto build() -> std::unique_ptr<basalt::widget>;
  // Takes finished images and video frames. Call it every frame while the view is open.
  auto update() -> void;

  [[nodiscard]] auto info() const -> detail_info const & { return info_; }
  [[nodiscard]] auto master_shown() const -> bool { return master_shown_; }
  [[nodiscard]] auto video() const -> video_player * { return video_.get(); }

  // The rows of the metadata panel as label and value, in order.
  [[nodiscard]] auto metadata_rows() const -> std::vector<std::pair<std::string, std::string>>;

private:
  auto marked() const -> bool;
  auto toggle_playback() -> void;

  wizard_controller &controller_;
  detail_info info_;
  image_loader &master_loader_;
  std::function<void()> wake_;
  callbacks handlers_;
  basalt::size size_;
  zoom_view *zoom_ = nullptr;
  basalt::label *zoom_label_ = nullptr;
  basalt::button *play_button_ = nullptr;
  std::unique_ptr<video_player> video_;
  bool master_shown_ = false;
  bool working_shown_ = false;
  bool video_started_ = false;
  video_player_factory videos_;
};

} // namespace kustavi::gui
