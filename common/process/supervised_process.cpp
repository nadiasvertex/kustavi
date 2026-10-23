#include "process/supervised_process.h"

#include "process/ring_buffer.h"

#include <array>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace kustavi::process {

struct supervised_process::state {
  state(child_process process, launch_options const &options)
      : child(std::move(process)), ready_prefix(options.ready_prefix),
        on_output(options.on_output), on_exit(options.on_exit), log(options.log_bytes) {}

  child_process child;
  std::string ready_prefix;
  std::function<void(std::string_view)> on_output;
  std::function<void(exit_info)> on_exit;

  mutable std::mutex mutex;
  std::condition_variable changed;
  ring_buffer log;
  std::optional<int> port;
  std::optional<std::string> failure;
  std::optional<int> exit_code;
  // Set when `launch` has returned the process, so that the waiter reports the exit from then on.
  bool launched = false;
  std::atomic<bool> shutdown_requested{false};

  std::vector<std::jthread> threads;

  auto record_line(std::string_view line) -> void {
    {
      std::scoped_lock lock{mutex};
      log.add_line(line);
    }
    if (on_output) {
      on_output(line);
    }
  }

  // The ready line is `<prefix> <port>`. A line that starts with the prefix and cannot be read
  // fails the launch.
  auto handle_stdout_line(std::string_view line) -> void {
    if (line.starts_with(ready_prefix)) {
      std::scoped_lock lock{mutex};
      if (!port && !failure) {
        auto rest = line.substr(ready_prefix.size());
        int value = 0;
        bool valid = !rest.empty() && rest.front() == ' ';
        if (valid) {
          rest.remove_prefix(1);
          while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t')) {
            rest.remove_suffix(1);
          }
          auto const *const end = rest.data() + rest.size();
          auto const parsed = std::from_chars(rest.data(), end, value);
          valid = parsed.ec == std::errc{} && parsed.ptr == end && value > 0 && value <= 65535;
        }
        if (valid) {
          port = value;
        } else {
          failure = "The process printed a ready line that cannot be read: \"" +
                    std::string{line} + "\".";
        }
        changed.notify_all();
        return;
      }
    }
    record_line(line);
  }

  // Splits what a pipe delivers into lines. A line may arrive in pieces.
  template <class Read, class OnLine> auto pump(Read read, OnLine on_line) -> void {
    std::array<char, 4096> buffer{};
    std::string pending;
    while (true) {
      auto const count = read(std::span<char>{buffer});
      if (count <= 0) {
        break;
      }
      pending.append(buffer.data(), static_cast<std::size_t>(count));
      std::size_t start = 0;
      for (auto end = pending.find('\n', start); end != std::string::npos;
           end = pending.find('\n', start)) {
        auto line = std::string_view{pending}.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
          line.remove_suffix(1);
        }
        on_line(line);
        start = end + 1;
      }
      pending.erase(0, start);
    }
    if (!pending.empty()) {
      on_line(pending);
    }
  }

  auto watch_exit() -> void {
    int const code = child.wait();
    std::function<void(exit_info)> report;
    {
      std::scoped_lock lock{mutex};
      exit_code = code;
      if (launched) {
        report = on_exit;
      }
    }
    changed.notify_all();
    if (report) {
      report({code, shutdown_requested.load()});
    }
  }
};

supervised_process::supervised_process(std::shared_ptr<state> shared) : state_(std::move(shared)) {}

auto supervised_process::launch(launch_options options)
    -> std::expected<std::unique_ptr<supervised_process>, std::string> {
  auto child = child_process::start(options.spawn);
  if (!child) {
    return std::unexpected("The process could not start: " + child.error());
  }
  auto shared = std::make_shared<state>(std::move(*child), options);
  auto *const raw = shared.get();
  raw->threads.emplace_back([raw] {
    raw->pump([raw](std::span<char> b) { return raw->child.read_stdout(b); },
              [raw](std::string_view line) { raw->handle_stdout_line(line); });
  });
  if (options.spawn.capture_stderr) {
    raw->threads.emplace_back([raw] {
      raw->pump([raw](std::span<char> b) { return raw->child.read_stderr(b); },
                [raw](std::string_view line) { raw->record_line(line); });
    });
  }
  raw->threads.emplace_back([raw] { raw->watch_exit(); });

  auto result = std::unique_ptr<supervised_process>{new supervised_process{shared}};
  std::unique_lock lock{raw->mutex};
  bool const done = raw->changed.wait_for(lock, options.ready_timeout, [raw] {
    return raw->port || raw->failure || raw->exit_code;
  });
  std::optional<std::string> error;
  if (raw->port) {
    // Ready. An exit that happened already is reported now, because the waiter did not.
    raw->launched = true;
    auto const code = raw->exit_code;
    auto const report = raw->on_exit;
    lock.unlock();
    if (code && report) {
      report({*code, false});
    }
    return result;
  }
  if (raw->failure) {
    error = *raw->failure;
  } else if (raw->exit_code) {
    error = "The process exited (code " + std::to_string(*raw->exit_code) +
            ") before signalling readiness.";
  } else if (!done) {
    error = "The process did not signal readiness in time.";
  }
  lock.unlock();
  // The destructor ends the process and joins the threads.
  result.reset();
  return std::unexpected(std::move(*error));
}

supervised_process::~supervised_process() {
  if (!state_) {
    return;
  }
  if (!has_exited()) {
    state_->child.kill();
  }
  state_->threads.clear();
}

auto supervised_process::port() const -> int {
  std::scoped_lock lock{state_->mutex};
  return state_->port.value_or(0);
}

auto supervised_process::pid() const -> std::int64_t { return state_->child.id(); }

auto supervised_process::log_text() const -> std::string {
  std::scoped_lock lock{state_->mutex};
  return state_->log.text();
}

auto supervised_process::request_shutdown() -> void { state_->shutdown_requested = true; }

auto supervised_process::shutdown_requested() const -> bool { return state_->shutdown_requested; }

auto supervised_process::has_exited() const -> bool {
  std::scoped_lock lock{state_->mutex};
  return state_->exit_code.has_value();
}

auto supervised_process::wait_for_exit(std::chrono::milliseconds timeout) -> std::optional<int> {
  std::unique_lock lock{state_->mutex};
  state_->changed.wait_for(lock, timeout, [this] { return state_->exit_code.has_value(); });
  return state_->exit_code;
}

auto supervised_process::terminate() -> void { state_->child.terminate(); }

auto supervised_process::kill() -> void { state_->child.kill(); }

} // namespace kustavi::process
