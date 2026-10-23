#pragma once

#include "core/kustavi_client.h"
#include "core/model_prep.h"
#include "core/ui_scope.h"

#include <functional>
#include <memory>

namespace kustavi::gui {

// Follows the background `EnsureModel` call that starts with the app: unknown, then downloading,
// then ready, or failed. All of it runs on the UI thread. Events from the worker arrive through
// the `ui_queue`.
class model_status {
public:
  explicit model_status(ui_queue &queue) : scope_(queue) {}

  // Called after each change, with the new state.
  auto on_change(std::function<void(model_prep_state const &)> handler) -> void {
    on_change_ = std::move(handler);
  }

  [[nodiscard]] auto state() const -> model_prep_state const & { return state_; }

  // Starts `EnsureModel` on `client`, which is idempotent in the back end. Replaces a call in
  // flight. A null client only stops the current call.
  auto start(std::shared_ptr<kustavi_client> client) -> void;
  // The download cancel button: ends the call and returns to unknown. The next `start` resumes.
  auto cancel_download() -> void;
  // The retry button: runs `EnsureModel` again.
  auto retry_download() -> void;

private:
  auto set_state(model_prep_state next) -> void;

  ui_scope scope_;
  std::shared_ptr<kustavi_client> client_;
  stream_subscription subscription_;
  model_prep_state state_{model_prep_unknown{}};
  std::function<void(model_prep_state const &)> on_change_;
};

} // namespace kustavi::gui
