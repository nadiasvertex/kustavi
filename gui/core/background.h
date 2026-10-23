#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace kustavi::gui {

// Runs a blocking task off the UI thread. The task posts its result to the `ui_queue`.
using background_executor = std::function<void(std::function<void()>)>;

// Runs each task on the calling thread, at once. Tests use it so that results arrive in a fixed
// order.
[[nodiscard]] auto inline_executor() -> background_executor;

// A fixed set of worker threads for the unary calls. Destroying the pool finishes the tasks that
// are queued and joins the threads.
class worker_pool {
public:
  explicit worker_pool(std::size_t threads = 2);
  worker_pool(worker_pool const &) = delete;
  auto operator=(worker_pool const &) -> worker_pool & = delete;
  ~worker_pool();

  auto submit(std::function<void()> task) -> void;
  // An executor that submits to this pool. It must not outlive the pool.
  [[nodiscard]] auto executor() -> background_executor;

private:
  std::mutex mutex_;
  std::condition_variable_any wake_;
  std::deque<std::function<void()>> tasks_;
  std::vector<std::jthread> threads_;
};

} // namespace kustavi::gui
