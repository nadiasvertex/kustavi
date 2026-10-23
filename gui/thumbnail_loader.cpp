#include "thumbnail_loader.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstring>
#include <optional>

namespace kustavi::gui {

namespace {

// Decodes at a quarter of the size, which is much faster for JPEG, and scales the rest of the way
// down so that the longer side is at most `edge`.
auto decode(std::filesystem::path const &path, int edge) -> std::optional<cv::Mat> {
  cv::Mat bgr = cv::imread(path.string(), cv::IMREAD_REDUCED_COLOR_4);
  if (bgr.empty()) {
    return std::nullopt;
  }
  if (int const longest = std::max(bgr.cols, bgr.rows); longest > edge) {
    double const factor = static_cast<double>(edge) / longest;
    cv::resize(bgr, bgr, {}, factor, factor, cv::INTER_AREA);
  }
  cv::Mat rgba;
  cv::cvtColor(bgr, rgba, cv::COLOR_BGR2RGBA);
  return rgba;
}

} // namespace

thumbnail_loader::thumbnail_loader(std::vector<std::filesystem::path> const &sources, int edge,
                                   unsigned workers, std::function<void()> on_ready)
    : on_ready_(std::move(on_ready)) {
  for (auto const &path : sources) {
    auto const rgba = decode(path, edge);
    if (!rgba) {
      continue;
    }
    decoded_image image;
    image.width = static_cast<std::uint32_t>(rgba->cols);
    image.height = static_cast<std::uint32_t>(rgba->rows);
    image.rgba.resize(static_cast<std::size_t>(rgba->total()) * 4);
    std::memcpy(image.rgba.data(), rgba->data, image.rgba.size());
    decoded_.push_back(std::move(image));
  }
  if (decoded_.empty()) {
    return;
  }
  for (unsigned i = 0; i < std::max(1U, workers); ++i) {
    workers_.emplace_back([this](std::stop_token stop) { work(stop); });
  }
}

auto thumbnail_loader::request(std::vector<std::size_t> items) -> void {
  {
    std::scoped_lock lock{mutex_};
    queue_ = std::move(items);
  }
  wake_.notify_all();
}

auto thumbnail_loader::take_ready() -> std::vector<result> {
  std::scoped_lock lock{mutex_};
  return std::exchange(ready_, {});
}

auto thumbnail_loader::work(std::stop_token stop) -> void {
  while (true) {
    std::size_t item = 0;
    {
      std::unique_lock lock{mutex_};
      if (!wake_.wait(lock, stop, [this] { return !queue_.empty(); })) {
        return;
      }
      // The queue is in the order the application wants the items, so the front goes first.
      item = queue_.front();
      queue_.erase(queue_.begin());
    }
    decoded_image const &source = decoded_[item % decoded_.size()];
    auto image = basalt::raster_image::create(source.width, source.height, source.rgba);
    {
      std::scoped_lock lock{mutex_};
      ready_.push_back({item, std::move(image)});
    }
    on_ready_();
  }
}

} // namespace kustavi::gui
