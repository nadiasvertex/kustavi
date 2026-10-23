#pragma once

#include "core/ui_queue.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace kustavi::gui {

// Lets a worker hand a result to an object on the UI thread without touching a destroyed object
// or a result that a newer operation has replaced. The object owns one scope. A worker captures
// the generation that it was started with and posts through `post`. When the object starts a new
// operation it calls `next_generation`, and results of the older ones are dropped. When the object
// is destroyed, so is the scope, and results still in the queue are dropped too.
class ui_scope {
  struct shared {
    std::uint64_t generation = 0;
  };

public:
  explicit ui_scope(ui_queue &queue) : queue_(queue), shared_(std::make_shared<shared>()) {}

  // UI thread only. Ends every operation that began before and returns the new generation.
  auto next_generation() -> std::uint64_t { return ++shared_->generation; }
  [[nodiscard]] auto generation() const -> std::uint64_t { return shared_->generation; }

  // A copy of the means to post. It does not refer to the scope, so a task that runs on a worker
  // can hold one after the object that owns the scope is gone.
  class poster {
  public:
    // Safe on any thread. `task` runs on the UI thread, and only if the scope still exists and
    // `generation` is still current.
    auto post(std::uint64_t generation, std::function<void()> task) const -> void {
      queue_->post([weak = weak_, generation, task = std::move(task)] {
        if (auto const state = weak.lock(); state && state->generation == generation) {
          task();
        }
      });
    }

  private:
    friend class ui_scope;
    poster(ui_queue *queue, std::weak_ptr<shared> weak) : queue_(queue), weak_(std::move(weak)) {}

    ui_queue *queue_;
    std::weak_ptr<shared> weak_;
  };

  [[nodiscard]] auto get_poster() const -> poster { return {&queue_, shared_}; }
  auto post(std::uint64_t generation, std::function<void()> task) const -> void {
    get_poster().post(generation, std::move(task));
  }

private:
  ui_queue &queue_;
  std::shared_ptr<shared> shared_;
};

} // namespace kustavi::gui
