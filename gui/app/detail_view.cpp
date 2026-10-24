#include "app/detail_view.h"

#include "app/ui_kit.h"
#include "core/format.h"

#include <format>

namespace kustavi::gui {

namespace {

using basalt::button_variant;
using basalt::text_role;
using basalt::text_tone;

constexpr auto panel_width = 320.0F;
// The longest side of the frames of a video, which is about what a window shows.
constexpr auto video_edge = 1600;
// A picture the loader cannot make from a file that is not an image falls back to the preview.
constexpr auto working_prefix = "working:";
constexpr auto master_prefix = "master:";

auto percent(double value) -> std::string {
  return std::format("{}%", static_cast<int>(std::lround(value * 100)));
}

} // namespace

detail_view::detail_view(wizard_controller &controller, detail_info info,
                         image_loader &master_loader, video_player_factory videos,
                         std::function<void()> wake, callbacks handlers, basalt::size size)
    : controller_(controller), info_(std::move(info)), master_loader_(master_loader),
      wake_(std::move(wake)), handlers_(std::move(handlers)), size_(size),
      videos_(std::move(videos)) {}

auto detail_view::marked() const -> bool { return controller_.is_marked(info_.image.id, info_.step); }

auto detail_view::metadata_rows() const -> std::vector<std::pair<std::string, std::string>> {
  std::vector<std::pair<std::string, std::string>> rows;
  auto const &image = info_.image;
  rows.emplace_back("Path", image.path);
  rows.emplace_back("Dimensions", std::format("{} × {}", image.width, image.height));
  rows.emplace_back("Size", format_bytes(image.size_bytes));
  if (image.video_duration) {
    rows.emplace_back("Length", format_media_duration(*image.video_duration));
  }
  if (image.taken) {
    rows.emplace_back("Taken", format_date_time(to_local_civil(*image.taken)));
  }
  if (image.gps) {
    rows.emplace_back("GPS", std::format("{:.5f}, {:.5f}", image.gps->latitude,
                                         image.gps->longitude));
  }
  if (info_.sharpness) {
    rows.emplace_back("Sharpness", std::format("{:.1f}", *info_.sharpness));
  }
  if (info_.exposure_score) {
    rows.emplace_back("Exposure", std::format("{:.2f}", *info_.exposure_score));
  }
  if (info_.junk_reason) {
    rows.emplace_back("Junk reason", *info_.junk_reason);
  }
  if (info_.junk_confidence) {
    rows.emplace_back("Confidence", percent(*info_.junk_confidence));
  }
  if (info_.video_reason) {
    rows.emplace_back("Video reason", *info_.video_reason);
  }
  if (info_.video_confidence) {
    rows.emplace_back("Confidence", percent(*info_.video_confidence));
  }
  for (auto const &[label, value] : info_.mappings) {
    if (!value.empty()) {
      rows.emplace_back(label, value);
    }
  }
  return rows;
}

auto detail_view::toggle_playback() -> void {
  if (!video_) {
    return;
  }
  if (video_->playing()) {
    video_->pause();
  } else {
    video_->play();
  }
  if (play_button_ != nullptr) {
    play_button_->set_label(video_->playing() ? "Pause" : "Play");
  }
}

auto detail_view::build() -> std::unique_ptr<basalt::widget> {
  auto const &image = info_.image;
  auto root = ui::row(24);
  root->set_padding(basalt::length_insets::uniform(basalt::fixed(24)));
  root->set_surface(basalt::box_surface{});
  root->set_cross_alignment(basalt::alignment::stretch);

  // The metadata.
  auto panel = ui::column(8);
  panel->add_child(ui::text(image.name, text_role::heading_small));
  for (auto const &[label, value] : metadata_rows()) {
    auto row = ui::column(1);
    row->add_child(ui::text(label, text_role::small, text_tone::subdued));
    row->add_child(ui::text(value));
    panel->add_child(std::move(row));
  }
  ui::fill(*panel, std::make_unique<basalt::box>());

  if (!info_.neighbors.empty()) {
    panel->add_child(ui::text("Nearby shots", text_role::small, text_tone::subdued));
    for (auto const &neighbor : info_.neighbors) {
      panel->add_child(ui::make_button(
          neighbor.name,
          [this, neighbor] {
            if (handlers_.open_neighbor) {
              handlers_.open_neighbor(neighbor);
            }
          },
          button_variant::tertiary));
    }
  }

  auto controls = ui::row(8);
  controls->add_child(ui::make_button("−", [this] { zoom_->zoom_out(); }));
  zoom_label_ = &controls->add_child(ui::text("100%"));
  controls->add_child(ui::make_button("+", [this] { zoom_->zoom_in(); }));
  controls->add_child(ui::make_button("Reset", [this] { zoom_->reset_zoom(); },
                                      button_variant::tertiary));
  panel->add_child(std::move(controls));

  if (image.is_video) {
    play_button_ = &panel->add_child(ui::make_button("Play", [this] { toggle_playback(); }));
    play_button_->set_visible(false);
  }

  auto &toggle = panel->add_child(
      std::make_unique<basalt::toggle>("Marked for deletion", marked()));
  toggle.set_enabled(info_.can_toggle);
  toggle.set_on_change([this](bool on) {
    if (on) {
      controller_.mark_image(info_.image.id);
    } else {
      controller_.keep_image(info_.image.id);
    }
  });
  panel->add_child(ui::make_button("Close", [this] {
    if (handlers_.close) {
      handlers_.close();
    }
  }));
  root->add(std::move(panel), basalt::box_item{.basis = basalt::fixed(panel_width),
                                                .max = basalt::fixed(panel_width)});

  // The picture.
  auto picture = std::make_unique<zoom_view>();
  zoom_ = picture.get();
  zoom_->set_source_size({static_cast<float>(image.width), static_cast<float>(image.height)});
  auto const picture_width = std::max(200.0F, size_.width - panel_width - 96);
  zoom_->set_preferred_size({picture_width, std::max(200.0F, size_.height - 96)});
  zoom_->set_on_zoom_change([this](float level) {
    if (zoom_label_ != nullptr) {
      zoom_label_->set_text(std::format("{}%", static_cast<int>(std::lround(level * 100))));
    }
  });
  // The dialog sizes itself to its content, so the picture asks for its room and does not wait
  // for spare space.
  root->add(std::move(picture), basalt::box_item{.grow = 1, .basis = basalt::fixed(picture_width)});

  // The preview comes first, then the master. A video has no master: its player makes frames.
  std::vector<image_request> wanted;
  if (!image.working_image_path.empty()) {
    wanted.push_back({working_prefix + image.id, image.working_image_path});
  }
  if (image.is_video) {
    if (videos_) {
      video_ = videos_(image.path, wake_, video_edge);
    }
  } else if (!image.path.empty()) {
    wanted.push_back({master_prefix + image.id, image.path});
  }
  master_loader_.request(this, std::move(wanted));
  return root;
}

auto detail_view::update() -> void {
  if (zoom_ == nullptr) {
    return;
  }
  for (auto &[key, picture] : master_loader_.take_ready()) {
    if (key == master_prefix + info_.image.id) {
      zoom_->set_image(std::move(picture));
      master_shown_ = true;
    } else if (key == working_prefix + info_.image.id && !master_shown_ && !video_started_) {
      // The preview does not replace a sharper picture that arrived first.
      zoom_->set_image(std::move(picture));
      working_shown_ = true;
    }
  }
  if (video_) {
    if (video_->failed()) {
      // The still image stays.
      video_.reset();
      return;
    }
    if (auto frame = video_->take_frame()) {
      video_started_ = true;
      zoom_->set_source_size({static_cast<float>(frame->width), static_cast<float>(frame->height)});
      zoom_->set_image(basalt::raster_image::create(frame->width, frame->height,
                                                    std::move(frame->rgba)));
      if (play_button_ != nullptr && !play_button_->visible()) {
        play_button_->set_visible(true);
        // Playing starts when the first frame is up.
        toggle_playback();
      }
    }
  }
}

} // namespace kustavi::gui
