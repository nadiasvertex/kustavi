#pragma once

#include "status.h"

#include <string>

namespace kustavi::gui {

using kustavi::status_code;

// A call that the back end did not complete. Operational failures are values. The GUI does not
// throw them.
struct backend_error {
  status_code code = status_code::internal;
  std::string message;

  // A path or resource that the call named does not exist.
  [[nodiscard]] auto not_found() const -> bool { return code == status_code::not_found; }
};

[[nodiscard]] auto message_of(backend_error const &error) -> std::string const &;

[[nodiscard]] auto to_backend_error(status const &failed) -> backend_error;

} // namespace kustavi::gui
