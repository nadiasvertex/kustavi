#include "app/video_player.h"

#if defined(KUSTAVI_HAVE_MDK)

#include "mdk/Player.h"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace kustavi::gui {

namespace {

// libmdk decodes and converts each frame to RGBA at the size that the view needs. The frames are
// copied into a buffer that the UI thread takes, and a frame that is not taken in time is
// replaced, so the playback keeps its time.
class mdk_video_player final : public video_player {
public:
  mdk_video_player(std::filesystem::path const &path, std::function<void()> wake, int max_edge)
      : wake_(std::move(wake)), max_edge_(max_edge) {
    player_.onFrame<mdk::VideoFrame>([this](mdk::VideoFrame &frame, int) {
      on_frame(frame);
      return 0;
    });
    player_.onMediaStatus([this](mdk::MediaStatus, mdk::MediaStatus next) {
      if (next & mdk::MediaStatus::End) {
        // The clip played to its end.
        playing_ = false;
      }
      if (next & mdk::MediaStatus::Invalid || next & mdk::MediaStatus::NoMedia) {
        failed_ = true;
        if (wake_) {
          wake_();
        }
      }
      return true;
    });
    player_.setMedia(path.string().c_str());
    // Opens the file and shows the first frame without playing.
    player_.prepare();
  }

  ~mdk_video_player() override {
    player_.onFrame<mdk::VideoFrame>(nullptr);
    player_.onMediaStatus(nullptr);
    player_.set(mdk::State::Stopped);
  }

  // The player changes state on its own threads, a moment after the call. The view asks at once, so
  // the answer is what was asked for.
  auto play() -> void override {
    playing_ = true;
    player_.set(mdk::State::Playing);
  }
  auto pause() -> void override {
    playing_ = false;
    player_.set(mdk::State::Paused);
  }
  [[nodiscard]] auto playing() const -> bool override { return playing_; }
  [[nodiscard]] auto failed() const -> bool override { return failed_; }

  [[nodiscard]] auto take_frame() -> std::optional<video_frame> override {
    std::scoped_lock lock{mutex_};
    return std::exchange(pending_, std::nullopt);
  }

private:
  auto on_frame(mdk::VideoFrame &frame) -> void {
    if (!frame) {
      return;
    }
    auto width = frame.width();
    auto height = frame.height();
    if (auto const longest = std::max(width, height); longest > max_edge_) {
      width = width * max_edge_ / longest;
      height = height * max_edge_ / longest;
    }
    auto const rgba = frame.to(mdk::PixelFormat::RGBA, width, height);
    if (!rgba) {
      return;
    }
    video_frame out;
    out.width = static_cast<std::uint32_t>(rgba.width());
    out.height = static_cast<std::uint32_t>(rgba.height());
    out.rgba.resize(std::size_t{out.width} * out.height * 4);
    auto const stride = static_cast<std::size_t>(rgba.bytesPerLine(0));
    auto const row = std::size_t{out.width} * 4;
    for (std::uint32_t y = 0; y < out.height; ++y) {
      std::copy_n(rgba.bufferData(0) + y * stride, row, out.rgba.data() + y * row);
    }
    {
      std::scoped_lock lock{mutex_};
      pending_ = std::move(out);
    }
    if (wake_) {
      wake_();
    }
  }

  mdk::Player player_;
  std::function<void()> wake_;
  int max_edge_;
  std::mutex mutex_;
  std::optional<video_frame> pending_;
  std::atomic<bool> failed_{false};
  std::atomic<bool> playing_{false};
};

} // namespace

auto default_video_player_factory() -> video_player_factory {
  return [](std::filesystem::path const &path, std::function<void()> wake, int max_edge)
             -> std::unique_ptr<video_player> {
    return std::make_unique<mdk_video_player>(path, std::move(wake), max_edge);
  };
}

} // namespace kustavi::gui

#else

namespace kustavi::gui {

auto default_video_player_factory() -> video_player_factory { return {}; }

} // namespace kustavi::gui

#endif
