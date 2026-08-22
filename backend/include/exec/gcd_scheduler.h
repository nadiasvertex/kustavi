#pragma once

#include <dispatch/dispatch.h>
#include <stdexec/execution.hpp>
#include <utility>

namespace kustavi::exec::gcd {

/**
 * @brief A simple GCD-based scheduler for stdexec pipelines.
 */
template <typename Receiver> struct gcd_operation {
  dispatch_queue_t queue_;
  Receiver receiver_;

  // stdexec calls start() to begin the asynchronous work
  void start() noexcept {
    // Retain the queue to ensure it stays alive while executing
    dispatch_retain(queue_);

    // Submit the work to the GCD queue
    dispatch_async(queue_, ^{
      // Check if the pipeline requested cancellation before executing
      if (stdexec::get_stop_token(stdexec::get_env(receiver_))
              .stop_requested()) {
        stdexec::set_stopped(std::move(receiver_));
      } else {
        stdexec::set_value(std::move(receiver_));
      }

      dispatch_release(queue_);
    });
  }
};

/**
 * @brief A sender that schedules work on a GCD queue.
 * Represents the work to be done. It is lazy and does nothing until connected.
 */
struct gcd_sender {
  using sender_concept = stdexec::sender_t;
  dispatch_queue_t queue_;

  // Explicitly declare what channels this sender can complete on.
  // GCD scheduling itself yields no values (void), but it can be cancelled.
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(),
                                     stdexec::set_stopped_t()>;

  // Connects this sender to a downstream receiver, producing an operation state
  template <stdexec::receiver Receiver> auto connect(Receiver rcvr) const {
    return gcd_operation<Receiver>{queue_, std::move(rcvr)};
  }

  [[nodiscard]] auto get_env() const noexcept { return stdexec::env<>{}; }
};

/**  The entry point interface that users interact with. */
class gcd_scheduler {
  dispatch_queue_t queue_;

public:
  gcd_scheduler()
      : queue_(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0)) {
  } // Default to main queue

  /**
   * Construct using any GCD queue (e.g., main queue, global concurrent queue,
   * serial queue)
   */
  explicit gcd_scheduler(dispatch_queue_t queue) : queue_(queue) {}

  /**
   *  Schedulers must be regularly embeddable and comparable
   */
  auto operator==(const gcd_scheduler &) const -> bool = default;

  /** The mandatory customization point for stdexec pipelines. */
  [[nodiscard]] auto schedule() const noexcept { return gcd_sender{queue_}; }
};

} // namespace kustavi::exec::gcd
