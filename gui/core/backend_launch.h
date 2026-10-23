#pragma once

#include "core/backend_error.h"
#include "core/format.h"
#include "process/supervised_process.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kustavi::gui {

// Where randomness comes from. Tests give a fixed sequence.
using random_source = std::function<std::uint32_t()>;
// Randomness from the operating system.
[[nodiscard]] auto system_random() -> random_source;

// The secret that the GUI and the back end share for one launch: 64 hexadecimal digits that go in
// the metadata of every call.
[[nodiscard]] auto generate_auth_token(random_source const &random) -> std::string;

// A name for this launch's log file in `temp_directory`, such as
// `kustavi-backend-20260824-143012-a1b2c3d4.log`.
[[nodiscard]] auto default_backend_log_file(std::filesystem::path const &temp_directory,
                                            civil_time now, random_source const &random)
    -> std::filesystem::path;

// The back end's path: the `KUSTAVI_BACKEND_PATH` override when it is set (and then nothing else
// is tried), otherwise `kustavi-backend` beside `executable`. Nothing when the file is missing.
[[nodiscard]] auto find_backend_binary(std::optional<std::string> const &override_path,
                                       std::filesystem::path const &executable)
    -> std::optional<std::filesystem::path>;

// The command line of `kustavi-backend serve`, without the program name.
[[nodiscard]] auto backend_arguments(std::string const &token,
                                     std::optional<std::filesystem::path> const &log_file,
                                     std::optional<std::int64_t> parent_pid)
    -> std::vector<std::string>;

struct backend_launch {
  std::filesystem::path binary;
  std::string token;
  std::optional<std::filesystem::path> log_file;
  // The back end ends itself if this process dies without a clean shutdown.
  std::optional<std::int64_t> parent_pid;
  std::chrono::milliseconds ready_timeout{30'000};
  // Called on a worker thread when the process exits, whether it was asked to or not.
  std::function<void(process::exit_info)> on_exit;
};

// The back-end process and the secret that its calls carry.
class backend_process {
public:
  backend_process(std::unique_ptr<process::supervised_process> process, std::string token,
                  std::optional<std::filesystem::path> log_file)
      : process_(std::move(process)), token_(std::move(token)), log_file_(std::move(log_file)) {}

  [[nodiscard]] auto port() const -> int { return process_->port(); }
  [[nodiscard]] auto token() const -> std::string const & { return token_; }
  [[nodiscard]] auto log_file() const -> std::optional<std::filesystem::path> const & {
    return log_file_;
  }
  [[nodiscard]] auto log_text() const -> std::string { return process_->log_text(); }
  [[nodiscard]] auto process() -> process::supervised_process & { return *process_; }

private:
  std::unique_ptr<process::supervised_process> process_;
  std::string token_;
  std::optional<std::filesystem::path> log_file_;
};

// Starts the back end and waits for its ready line. It blocks, so call it off the UI thread.
[[nodiscard]] auto launch_backend(backend_launch options)
    -> std::expected<std::unique_ptr<backend_process>, backend_startup_failed>;

} // namespace kustavi::gui
