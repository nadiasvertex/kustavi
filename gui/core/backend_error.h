#pragma once

#include <grpcpp/support/status.h>

#include <string>
#include <variant>

namespace kustavi::gui {

// The back-end process died, or the transport dropped during a call.
struct backend_crashed {
  std::string message;
};

// A call ended with a status other than cancelled, not found or unavailable.
struct backend_rpc {
  grpc::StatusCode code = grpc::StatusCode::UNKNOWN;
  std::string message;
};

// The back end could not be launched or did not signal that it was ready.
struct backend_startup_failed {
  std::string message;
};

// A path or resource that a call named does not exist.
struct backend_not_found {
  std::string message;
};

// Operational failures are values. The GUI does not throw them.
using backend_error =
    std::variant<backend_crashed, backend_rpc, backend_startup_failed, backend_not_found>;

[[nodiscard]] auto message_of(backend_error const &error) -> std::string const &;

// NOT_FOUND is `backend_not_found`. UNAVAILABLE during a call is a crash. Any other status is
// `backend_rpc` with the status unchanged.
[[nodiscard]] auto map_grpc_status(grpc::Status const &status) -> backend_error;

} // namespace kustavi::gui
