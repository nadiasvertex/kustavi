#include "core/model_status.h"

namespace kustavi::gui {

auto model_status::set_state(model_prep_state next) -> void {
  state_ = std::move(next);
  if (on_change_) {
    on_change_(state_);
  }
}

auto model_status::start(std::shared_ptr<kustavi_client> client) -> void {
  subscription_.cancel();
  client_ = std::move(client);
  auto const generation = scope_.next_generation();
  set_state(model_prep_unknown{});
  if (!client_) {
    return;
  }
  stream_handlers<ModelEvent> handlers;
  handlers.on_event = [this, generation](ModelEvent const &event) {
    // The state is built on the worker. It is plain data.
    if (auto next = model_prep_from(event)) {
      scope_.post(generation, [this, next = std::move(*next)]() mutable {
        set_state(std::move(next));
      });
    }
  };
  handlers.on_done = [this, generation](std::optional<backend_error> error) {
    // A crash is handled where the back end is restarted, which starts this call again.
    if (!error || std::holds_alternative<backend_crashed>(*error)) {
      return;
    }
    scope_.post(generation, [this, message = message_of(*error)] {
      set_state(model_prep_failed{message});
    });
  };
  subscription_ = client_->ensure_model(std::move(handlers));
}

auto model_status::cancel_download() -> void {
  subscription_.cancel();
  scope_.next_generation();
  set_state(model_prep_unknown{});
}

auto model_status::retry_download() -> void { start(client_); }

} // namespace kustavi::gui
