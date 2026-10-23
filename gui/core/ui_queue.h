#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <vector>

namespace kustavi::gui {

// Hands work from other threads to the UI thread. A worker posts a task and the queue calls its
// wake function, which ends the event loop's wait. The loop calls `drain` on the UI thread and the
// tasks run there. Only the UI thread builds widgets and changes the app state, so a task is the
// one safe way for a worker to cause that.
class ui_queue {
public:
  // `wake` is called from the posting thread after each post. It must be safe on any thread.
  explicit ui_queue(std::function<void()> wake = {}) : wake_(std::move(wake)) {}

  ui_queue(ui_queue const &) = delete;
  auto operator=(ui_queue const &) -> ui_queue & = delete;

  // Safe on any thread.
  auto post(std::function<void()> task) -> void;

  // Runs the tasks that were posted before the call, in order, on the calling thread. A task
  // that posts another task leaves it for the next call, so a task cannot keep the loop busy for
  // ever. Returns how many tasks ran.
  auto drain() -> std::size_t;

  // True when a task waits. The loop uses it to ask for another pass.
  [[nodiscard]] auto pending() const -> bool;

private:
  std::function<void()> wake_;
  mutable std::mutex mutex_;
  std::vector<std::function<void()>> tasks_;
};

} // namespace kustavi::gui
