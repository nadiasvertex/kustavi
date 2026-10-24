#include "app/image_loader.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <ranges>
#include <utility>
#include <vector>

namespace kustavi::gui {

namespace {

// Decodes at a quarter of the size when the file is much larger than `edge`, which is fast for
// JPEG, and scales the rest of the way down so that the longer side is at most `edge`.
auto decode(std::filesystem::path const &path, int edge, bool full_resolution)
    -> basalt::raster_image_ptr {
  cv::Mat bgr;
  if (!full_resolution) {
    bgr = cv::imread(path.string(), cv::IMREAD_REDUCED_COLOR_4);
  }
  if (bgr.empty()) {
    bgr = cv::imread(path.string(), cv::IMREAD_COLOR);
  }
  if (bgr.empty()) {
    return nullptr;
  }
  if (int const longest = std::max(bgr.cols, bgr.rows); longest > edge) {
    double const factor = static_cast<double>(edge) / longest;
    cv::resize(bgr, bgr, {}, factor, factor, cv::INTER_AREA);
  }
  cv::Mat rgba;
  cv::cvtColor(bgr, rgba, cv::COLOR_BGR2RGBA);
  std::vector<std::uint8_t> bytes(rgba.data, rgba.data + rgba.total() * 4);
  return basalt::raster_image::create(static_cast<std::uint32_t>(rgba.cols),
                                      static_cast<std::uint32_t>(rgba.rows), std::move(bytes));
}

} // namespace

image_loader::image_loader(int edge, unsigned workers, std::function<void()> on_ready,
                           bool full_resolution)
    : edge_(edge), full_resolution_(full_resolution), on_ready_(std::move(on_ready)) {
  for (unsigned i = 0; i < std::max(1U, workers); ++i) {
    workers_.emplace_back([this](std::stop_token stop) { work(stop); });
  }
}

image_loader::~image_loader() {
  for (auto &worker : workers_) {
    worker.request_stop();
  }
  wake_.notify_all();
}

auto image_loader::request(void const *owner, std::vector<image_request> wanted) -> void {
  {
    std::scoped_lock lock{mutex_};
    std::erase_if(queue_, [owner](queued const &entry) { return entry.owner == owner; });
    // The next to decode is at the back, so the first of `wanted` goes in last. The new request
    // goes in front of the others, which makes the grid that was touched last the first served.
    std::vector<queued> fresh;
    fresh.reserve(wanted.size());
    for (auto &item : wanted | std::views::reverse) {
      fresh.push_back({owner, std::move(item)});
    }
    queue_.insert(queue_.end(), std::make_move_iterator(fresh.begin()),
                  std::make_move_iterator(fresh.end()));
  }
  wake_.notify_all();
}

auto image_loader::take_ready() -> std::vector<loaded_image> {
  std::scoped_lock lock{mutex_};
  return std::exchange(ready_, {});
}

auto image_loader::work(std::stop_token stop) -> void {
  while (true) {
    image_request next;
    {
      std::unique_lock lock{mutex_};
      if (!wake_.wait(lock, stop, [this] { return !queue_.empty(); })) {
        return;
      }
      next = std::move(queue_.back().request);
      queue_.pop_back();
    }
    auto image = decode(next.path, edge_, full_resolution_);
    if (image == nullptr) {
      continue;
    }
    {
      std::scoped_lock lock{mutex_};
      ready_.push_back({std::move(next.key), std::move(image)});
    }
    if (on_ready_) {
      on_ready_();
    }
  }
}

} // namespace kustavi::gui
