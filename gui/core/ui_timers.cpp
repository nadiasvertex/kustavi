#include "core/ui_timers.h"

#include <algorithm>
#include <vector>

namespace kustavi::gui {

auto ui_timers::after(std::chrono::milliseconds delay, std::function<void()> task) -> timer_id {
  auto const id = next_id_++;
  timers_.emplace(id, entry{clock_() + delay, std::move(task)});
  return id;
}

auto ui_timers::cancel(timer_id id) -> void { timers_.erase(id); }

auto ui_timers::run_due() -> std::size_t {
  auto const now = clock_();
  std::vector<std::pair<std::chrono::steady_clock::time_point, timer_id>> due;
  for (auto const &[id, timer] : timers_) {
    if (timer.due <= now) {
      due.emplace_back(timer.due, id);
    }
  }
  std::ranges::sort(due);
  std::size_t ran = 0;
  for (auto const &[when, id] : due) {
    // An earlier task may have cancelled this one.
    auto const found = timers_.find(id);
    if (found == timers_.end()) {
      continue;
    }
    auto task = std::move(found->second.task);
    timers_.erase(found);
    task();
    ++ran;
  }
  return ran;
}

auto ui_timers::next_due() const -> std::optional<std::chrono::milliseconds> {
  if (timers_.empty()) {
    return std::nullopt;
  }
  auto first = timers_.begin()->second.due;
  for (auto const &entry : timers_) {
    first = std::min(first, entry.second.due);
  }
  return std::max(std::chrono::milliseconds{0},
                  std::chrono::duration_cast<std::chrono::milliseconds>(first - clock_()));
}

} // namespace kustavi::gui
