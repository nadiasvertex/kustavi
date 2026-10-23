#pragma once

#include "process/child_process.h"

#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace kustavi::process {

// How a supervised process ended.
struct exit_info {
  int code = 0;
  // True when `request_shutdown` ran first, so the exit was asked for and is not a crash.
  bool shutdown_requested = false;
};

struct launch_options {
  spawn_options spawn;
  // The line the process prints on stdout when it is ready is `<ready_prefix> <port>`.
  std::string ready_prefix = "KUSTAVI-READY";
  std::chrono::milliseconds ready_timeout{30'000};
  // Capacity of the log that keeps the process's output.
  std::size_t log_bytes = 256 * 1024;
  // Called with each output line other than the ready line. It runs on a reader thread.
  std::function<void(std::string_view line)> on_output;
  // Called once when the process has exited, on the thread that waits for it. It must not destroy
  // the `supervised_process`.
  std::function<void(exit_info)> on_exit;
};

// A child process that prints a ready line with a port, whose output is kept in a bounded log and
// whose exit is reported. This is the launch protocol of the back end.
class supervised_process {
public:
  // Starts the process and waits for its ready line. It blocks, so call it off the UI thread. The
  // error is a sentence about what went wrong: the process could not start, ended before it was
  // ready, printed a ready line that cannot be read, or was not ready in time.
  [[nodiscard]] static auto launch(launch_options options)
      -> std::expected<std::unique_ptr<supervised_process>, std::string>;

  supervised_process(supervised_process const &) = delete;
  auto operator=(supervised_process const &) -> supervised_process & = delete;
  // Ends the process if it still runs, and waits for the threads that watch it.
  ~supervised_process();

  [[nodiscard]] auto port() const -> int;
  [[nodiscard]] auto pid() const -> std::int64_t;
  // The recent output, including what the process wrote to stderr.
  [[nodiscard]] auto log_text() const -> std::string;

  // Marks the coming exit as asked for. Call it before telling the process to stop.
  auto request_shutdown() -> void;
  [[nodiscard]] auto shutdown_requested() const -> bool;

  [[nodiscard]] auto has_exited() const -> bool;
  // The exit code if the process exits within `timeout`.
  [[nodiscard]] auto wait_for_exit(std::chrono::milliseconds timeout) -> std::optional<int>;
  auto terminate() -> void;
  auto kill() -> void;

private:
  struct state;
  explicit supervised_process(std::shared_ptr<state> shared);
  std::shared_ptr<state> state_;
};

} // namespace kustavi::process
