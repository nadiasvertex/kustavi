#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace kustavi {

/** A single-producer, single-consumer queue for streaming events from a
 * worker thread to the gRPC handler thread.
 *
 * The producer pushes events (from scheduler threads or a worker thread)
 * and calls `close()` exactly once when no further events will arrive.
 * The consumer blocks in `wait()` on a short timeout so it can observe
 * client cancellation between events.
 */
template <class T> class event_queue {
public:
  void push(T event) {
    {
      std::lock_guard lock(mutex_);
      queue_.push_back(std::move(event));
    }
    cv_.notify_one();
  }

  void close() {
    {
      std::lock_guard lock(mutex_);
      closed_ = true;
    }
    cv_.notify_all();
  }

  /** Blocks until an event is available or the timeout elapses.
   * @return The next event, or std::nullopt when the queue is closed and
   * drained (or the timeout elapsed with no event).
   */
  auto wait(std::chrono::milliseconds timeout) -> std::optional<T> {
    std::unique_lock lock(mutex_);
    cv_.wait_for(lock, timeout, [this] { return !queue_.empty() || closed_; });
    if (queue_.empty()) {
      return std::nullopt;
    }
    T event = std::move(queue_.front());
    queue_.pop_front();
    return event;
  }

  auto is_closed() const -> bool {
    std::lock_guard lock(mutex_);
    return closed_;
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<T> queue_;
  bool closed_ = false;
};

} // namespace kustavi
