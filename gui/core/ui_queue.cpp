#include "core/ui_queue.h"

#include <utility>

namespace kustavi::gui {

auto ui_queue::post(std::function<void()> task) -> void {
  {
    std::scoped_lock lock{mutex_};
    tasks_.push_back(std::move(task));
  }
  if (wake_) {
    wake_();
  }
}

auto ui_queue::drain() -> std::size_t {
  std::vector<std::function<void()>> batch;
  {
    std::scoped_lock lock{mutex_};
    batch.swap(tasks_);
  }
  for (auto &task : batch) {
    task();
  }
  return batch.size();
}

auto ui_queue::pending() const -> bool {
  std::scoped_lock lock{mutex_};
  return !tasks_.empty();
}

} // namespace kustavi::gui
