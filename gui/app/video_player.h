#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace kustavi::gui {

// One decoded picture of a video, as 8-bit RGBA with no padding between rows.
struct video_frame {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgba;
};

// Plays a video file into pictures that Basalt can paint. The player decodes on its own threads and
// keeps only the newest frame, so a slow screen drops frames and never queues them. It is used on
// the UI thread, apart from the wake function that it calls from a decoder thread.
class video_player {
public:
  virtual ~video_player() = default;

  virtual auto play() -> void = 0;
  virtual auto pause() -> void = 0;
  [[nodiscard]] virtual auto playing() const -> bool = 0;
  // The newest frame since the last call, if there is one.
  [[nodiscard]] virtual auto take_frame() -> std::optional<video_frame> = 0;
  // The file could not be opened or decoded.
  [[nodiscard]] virtual auto failed() const -> bool = 0;
};

// Opens `path` and starts it paused. `wake` is called from a decoder thread when a frame is ready.
// `max_edge` is the longest side of the frames that the player makes, in pixels.
using video_player_factory = std::function<std::unique_ptr<video_player>(
    std::filesystem::path const &path, std::function<void()> wake, int max_edge)>;

// The player of this build. It is empty when the build has no video library, and the detail view
// then shows the still image of the video.
[[nodiscard]] auto default_video_player_factory() -> video_player_factory;

} // namespace kustavi::gui
