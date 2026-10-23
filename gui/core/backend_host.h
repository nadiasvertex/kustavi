#pragma once

#include "core/background.h"
#include "core/backend_launch.h"
#include "core/kustavi_client.h"
#include "core/ui_scope.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace kustavi::gui {

// A running back end as the GUI uses it.
struct backend_connection {
  std::shared_ptr<kustavi_client> client;
  // The recent output of the back-end process.
  std::function<std::string()> log_text;
  // Asks the back end to stop and waits for it. It blocks, so call it off the UI thread.
  std::function<void()> stop;
};

// Starts a back end. `on_unexpected_exit` is called, on any thread, if the process ends without
// `stop` having been called. It blocks, so call it off the UI thread.
using backend_connector = std::function<std::expected<backend_connection, backend_startup_failed>(
    std::function<void()> on_unexpected_exit)>;

// Where the back end stands.
enum class backend_state { stopped, starting, ready, crashed, failed };

// Owns the life of the back end: starting it off the UI thread, telling the app when it is ready
// or has died, restarting it, and stopping it at exit. It runs on the UI thread; the connector runs
// on the executor.
class backend_host {
public:
  struct handlers {
    // The back end answers. Called again after a restart, with the new client.
    std::function<void(std::shared_ptr<kustavi_client>)> on_ready;
    // The process ended without being asked to.
    std::function<void()> on_crashed;
    // The back end could not be started.
    std::function<void(std::string message)> on_failed;
  };

  backend_host(ui_queue &queue, background_executor executor, backend_connector connector);
  backend_host(backend_host const &) = delete;
  auto operator=(backend_host const &) -> backend_host & = delete;

  auto set_handlers(handlers h) -> void { handlers_ = std::move(h); }

  [[nodiscard]] auto state() const -> backend_state { return state_; }
  // The output of the current or the last back-end process; empty before the first start.
  [[nodiscard]] auto log_text() const -> std::string;

  // Starts the back end. Does nothing while it is starting or ready.
  auto start() -> void;
  // Stops what is left of the previous process and starts a new one.
  auto restart() -> void;
  // Stops the back end and calls `done` on the UI thread when it is gone. The host starts nothing
  // after this.
  auto quit(std::function<void()> done) -> void;

private:
  auto launch() -> void;
  auto discard_connection() -> void;

  ui_scope scope_;
  background_executor executor_;
  backend_connector connector_;
  handlers handlers_;
  backend_state state_ = backend_state::stopped;
  std::shared_ptr<backend_connection> connection_;
  std::string last_log_;
  bool quitting_ = false;
};

// The production connector: runs `kustavi-backend serve` and connects over loopback gRPC.
struct process_connector_options {
  // The `KUSTAVI_BACKEND_PATH` override, if set.
  std::optional<std::string> binary_override;
  // The GUI's own path, which the back end sits beside.
  std::filesystem::path executable;
  std::filesystem::path temp_directory;
  std::optional<std::int64_t> parent_pid;
  std::chrono::milliseconds probe_timeout{10'000};
  std::chrono::milliseconds stop_timeout{5'000};
};

[[nodiscard]] auto make_process_connector(process_connector_options options) -> backend_connector;

} // namespace kustavi::gui
