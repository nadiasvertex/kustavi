#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <utility>

namespace kustavi {

/** How a call to the engine ended. The values keep the meaning that they had
 * as gRPC status codes, which the engine's callers still rely on. */
enum class status_code {
  ok,
  /// The caller cancelled the call, or a stream consumer stopped taking events.
  cancelled,
  /// The request holds a value that no pass can use.
  invalid_argument,
  /// A path or a resource that the request names does not exist.
  not_found,
  /// The engine is in the wrong state for the call, such as no open session
  /// or another pass already running.
  failed_precondition,
  /// A pass or the store failed.
  internal,
};

/** The result of an engine call, with a message for anything but `ok`. */
class status {
public:
  status() = default;
  status(status_code code, std::string message)
      : code_(code), message_(std::move(message)) {}

  [[nodiscard]] auto ok() const -> bool { return code_ == status_code::ok; }
  [[nodiscard]] auto error_code() const -> status_code { return code_; }
  [[nodiscard]] auto error_message() const -> const std::string & {
    return message_;
  }

private:
  status_code code_ = status_code::ok;
  std::string message_;
};

inline auto ok_status() -> status { return {}; }

/** What a caller can do to a call that is running: ask it to stop. A streaming
 * pass looks at this between events and ends with `cancelled`. */
class request_context {
public:
  void cancel() { cancelled_.store(true); }
  [[nodiscard]] auto is_cancelled() const -> bool { return cancelled_.load(); }

private:
  std::atomic<bool> cancelled_{false};
};

/** Where a streaming pass puts its events. `write` returns false when the
 * consumer wants no more, and the pass then stops with `cancelled`. */
template <class Event> class event_writer {
public:
  explicit event_writer(std::function<bool(const Event &)> sink)
      : sink_(std::move(sink)) {}

  auto write(const Event &event) -> bool { return sink_(event); }

private:
  std::function<bool(const Event &)> sink_;
};

} // namespace kustavi
