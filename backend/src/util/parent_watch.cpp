#include "util/parent_watch.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <stop_token>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace kustavi::util {
namespace {

using namespace std::chrono_literals;

constexpr auto poll_interval = 500ms;

[[noreturn]] void force_exit(std::int64_t parent_pid) {
  spdlog::warn("launching process {} is gone; forcing back-end exit",
               parent_pid);
  spdlog::shutdown(); // flush the log file before the abrupt exit
  std::_Exit(0);
}

#if defined(_WIN32)

void watch_loop(const std::stop_token &stop, std::int64_t parent_pid) {
  const HANDLE parent =
      ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(parent_pid));
  if (parent == nullptr) {
    force_exit(parent_pid); // already gone (or never existed)
  }
  while (!stop.stop_requested()) {
    if (::WaitForSingleObject(parent, 0) == WAIT_OBJECT_0) {
      ::CloseHandle(parent);
      force_exit(parent_pid);
    }
    std::this_thread::sleep_for(poll_interval);
  }
  ::CloseHandle(parent);
}

#else

void watch_loop(const std::stop_token &stop, std::int64_t parent_pid) {
  const auto parent = static_cast<::pid_t>(parent_pid);
  while (!stop.stop_requested()) {
    // The parent exiting reparents us to pid 1 (init/launchd); a recycled
    // or vanished PID stops resolving to a live process.
    const bool reparented = ::getppid() != parent;
    const bool gone = ::kill(parent, 0) == -1 && errno == ESRCH;
    if (reparented || gone) {
      force_exit(parent_pid);
    }
    std::this_thread::sleep_for(poll_interval);
  }
}

#endif

} // namespace

void watch_parent_and_exit(std::int64_t parent_pid) {
  if (parent_pid <= 0) {
    return;
  }
  // Function-local static: one watcher per process, joined (via stop
  // request) only during normal static teardown, which the loop never
  // blocks on for more than one poll interval.
  static std::jthread watcher(
      [parent_pid](const std::stop_token &stop) -> void {
        watch_loop(stop, parent_pid);
      });
}

} // namespace kustavi::util
