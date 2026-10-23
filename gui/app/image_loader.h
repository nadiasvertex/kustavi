#pragma once

#include "basalt/image.h"

#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace kustavi::gui {

struct image_request {
  // What the caller calls the image. The result carries it back.
  std::string key;
  std::filesystem::path path;
};

struct loaded_image {
  std::string key;
  basalt::raster_image_ptr image;
};

// Decodes image files into `basalt::raster_image` on a few worker threads. The caller says which
// images it wants, in the order it wants them, and takes the finished ones on the UI thread. A new
// request replaces the previous one, so images that scrolled out of view are dropped before they
// are decoded.
class image_loader {
public:
  // `edge` is the longest side of a decoded image, in pixels. `on_ready` is called on a worker
  // after an image is finished, to wake the event loop.
  image_loader(int edge, unsigned workers, std::function<void()> on_ready);
  image_loader(image_loader const &) = delete;
  auto operator=(image_loader const &) -> image_loader & = delete;
  ~image_loader();

  // Replaces the queue.
  auto request(std::vector<image_request> wanted) -> void;
  // The images finished since the last call. An image that cannot be read is not reported.
  [[nodiscard]] auto take_ready() -> std::vector<loaded_image>;

private:
  auto work(std::stop_token stop) -> void;

  int edge_;
  std::function<void()> on_ready_;
  std::mutex mutex_;
  std::condition_variable_any wake_;
  std::vector<image_request> queue_;
  std::vector<loaded_image> ready_;
  std::vector<std::jthread> workers_;
};

} // namespace kustavi::gui
