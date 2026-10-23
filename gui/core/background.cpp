#include "core/background.h"

#include <algorithm>

namespace kustavi::gui {

auto inline_executor() -> background_executor {
  return [](std::function<void()> task) { task(); };
}

worker_pool::worker_pool(std::size_t threads) {
  for (std::size_t i = 0; i < std::max<std::size_t>(1, threads); ++i) {
    threads_.emplace_back([this](std::stop_token stop) {
      while (true) {
        std::function<void()> task;
        {
          std::unique_lock lock{mutex_};
          // The wait ends on stop only when nothing is queued, so queued tasks finish first.
          wake_.wait(lock, stop, [this] { return !tasks_.empty(); });
          if (tasks_.empty()) {
            return;
          }
          task = std::move(tasks_.front());
          tasks_.pop_front();
        }
        task();
      }
    });
  }
}

worker_pool::~worker_pool() {
  for (auto &thread : threads_) {
    thread.request_stop();
  }
  wake_.notify_all();
}

auto worker_pool::submit(std::function<void()> task) -> void {
  {
    std::scoped_lock lock{mutex_};
    tasks_.push_back(std::move(task));
  }
  wake_.notify_one();
}

auto worker_pool::executor() -> background_executor {
  return [this](std::function<void()> task) { submit(std::move(task)); };
}

} // namespace kustavi::gui
