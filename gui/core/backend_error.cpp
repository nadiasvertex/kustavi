#include "core/backend_error.h"

namespace kustavi::gui {

auto message_of(backend_error const &error) -> std::string const & {
  return std::visit([](auto const &value) -> std::string const & { return value.message; }, error);
}

auto map_grpc_status(grpc::Status const &status) -> backend_error {
  switch (status.error_code()) {
  case grpc::StatusCode::NOT_FOUND:
    return backend_not_found{status.error_message()};
  case grpc::StatusCode::UNAVAILABLE:
    return backend_crashed{status.error_message()};
  default:
    return backend_rpc{status.error_code(), status.error_message()};
  }
}

} // namespace kustavi::gui
