#pragma once

#include "basalt/image.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace kustavi::gui {

// Supplies one image for each item index of a large collection, off the UI thread.
//
// The source photos are decoded once, at a reduced size. Item `i` then gets a copy of source
// `i % source_count()` with an image id of its own, as a separate decoded file would have. That
// makes a few photos stand in for thousands of files, and it makes the renderer's texture cache and
// the grid's cell reuse behave as they will with a real collection.
class thumbnail_loader {
public:
  struct result {
    std::size_t item = 0;
    basalt::raster_image_ptr image;
  };

  // Decodes every readable file in `sources` and starts `workers` threads. `on_ready` runs on a
  // worker thread each time results are waiting, so it must be thread-safe.
  thumbnail_loader(std::vector<std::filesystem::path> const &sources, int edge, unsigned workers,
                   std::function<void()> on_ready);

  [[nodiscard]] auto source_count() const -> std::size_t { return decoded_.size(); }

  // Replaces the queue of items to load, so items that scrolled away are dropped. Work that has
  // started finishes.
  auto request(std::vector<std::size_t> items) -> void;
  // The images finished since the last call.
  [[nodiscard]] auto take_ready() -> std::vector<result>;

private:
  struct decoded_image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
  };

  auto work(std::stop_token stop) -> void;

  std::vector<decoded_image> decoded_;
  std::function<void()> on_ready_;
  std::mutex mutex_;
  std::condition_variable_any wake_;
  std::vector<std::size_t> queue_;
  std::vector<result> ready_;
  // The workers stop and join before the members above are destroyed.
  std::vector<std::jthread> workers_;
};

} // namespace kustavi::gui
