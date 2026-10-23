#include "core/kustavi_client.h"

#include <utility>

namespace kustavi::gui {

auto stream_subscription::control::cancel() -> void {
  // The call is cancelled under the lock, because the worker clears `cancel_call` under the same
  // lock before the call's context goes away.
  std::scoped_lock lock{mutex};
  cancelled = true;
  if (cancel_call) {
    cancel_call();
  }
}

auto stream_subscription::control::is_cancelled() -> bool {
  std::scoped_lock lock{mutex};
  return cancelled;
}

auto stream_subscription::operator=(stream_subscription &&other) noexcept
    -> stream_subscription & {
  if (this != &other) {
    cancel();
    worker_ = std::move(other.worker_);
    control_ = std::move(other.control_);
  }
  return *this;
}

auto stream_subscription::cancel() -> void {
  if (control_) {
    control_->cancel();
  }
}

} // namespace kustavi::gui
