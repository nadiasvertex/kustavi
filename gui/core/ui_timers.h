#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>

namespace kustavi::gui {

// One-shot timers for the UI thread. The event loop asks `next_due` how long it may sleep and calls
// `run_due` after it wakes. The clock is a parameter, so a test moves time by hand.
class ui_timers {
public:
  using clock_fn = std::function<std::chrono::steady_clock::time_point()>;
  using timer_id = std::uint64_t;

  explicit ui_timers(clock_fn clock = [] { return std::chrono::steady_clock::now(); })
      : clock_(std::move(clock)) {}

  // Runs `task` once, `delay` from now, on the thread that calls `run_due`.
  auto after(std::chrono::milliseconds delay, std::function<void()> task) -> timer_id;
  // A timer that has run or does not exist is ignored.
  auto cancel(timer_id id) -> void;
  // Runs the timers that are due, oldest first. A timer that a task starts waits for its time.
  // Returns how many ran.
  auto run_due() -> std::size_t;
  // How long until the first timer is due; nothing when there are none. Zero when one is due.
  [[nodiscard]] auto next_due() const -> std::optional<std::chrono::milliseconds>;

private:
  struct entry {
    std::chrono::steady_clock::time_point due;
    std::function<void()> task;
  };

  clock_fn clock_;
  timer_id next_id_ = 1;
  std::map<timer_id, entry> timers_;
};

} // namespace kustavi::gui
