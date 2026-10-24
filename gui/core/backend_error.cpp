#include "core/backend_error.h"

namespace kustavi::gui {

auto message_of(backend_error const &error) -> std::string const & { return error.message; }

auto to_backend_error(status const &failed) -> backend_error {
  return {failed.error_code(), failed.error_message()};
}

} // namespace kustavi::gui
