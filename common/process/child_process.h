#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace kustavi::process {

struct spawn_options {
  std::filesystem::path binary;
  std::vector<std::string> args;
  // When false the child's stderr is the parent's own stderr, and `read_stderr` returns zero.
  bool capture_stderr = true;
};

// A child process with its standard output and, usually, its standard error on pipes, and its
// standard input closed. The reading and waiting functions block, so a caller gives each its own
// thread. `terminate` and `kill` may be called from any thread while another thread reads or
// waits. Destroying the object closes the pipes and leaves the child running.
class child_process {
public:
  [[nodiscard]] static auto start(spawn_options const &options)
      -> std::expected<child_process, std::string>;

  child_process(child_process &&) noexcept;
  auto operator=(child_process &&) noexcept -> child_process &;
  child_process(child_process const &) = delete;
  auto operator=(child_process const &) -> child_process & = delete;
  ~child_process();

  [[nodiscard]] auto id() const -> std::int64_t;

  // Reads what is there, at least one byte. Zero means that the pipe is closed, and a negative
  // value means that reading failed.
  [[nodiscard]] auto read_stdout(std::span<char> buffer) -> std::ptrdiff_t;
  [[nodiscard]] auto read_stderr(std::span<char> buffer) -> std::ptrdiff_t;

  // Waits for the child to exit and returns its exit code. A child that a signal ended has the
  // code 128 plus the signal number. Call it from one thread only.
  [[nodiscard]] auto wait() -> int;

  // Asks the child to stop, which it may handle (SIGTERM). Windows has no such request, so the
  // child ends at once there.
  auto terminate() -> void;
  // Ends the child at once.
  auto kill() -> void;

  // The path of the running executable.
  [[nodiscard]] static auto current_executable() -> std::filesystem::path;
  [[nodiscard]] static auto current_process_id() -> std::int64_t;

private:
  struct impl;
  explicit child_process(std::unique_ptr<impl> state);
  std::unique_ptr<impl> impl_;
};

} // namespace kustavi::process
