#include "kustavi_service.h"

#include "paths.h"

#include <spdlog/spdlog.h>

#include <utility>

namespace fs = std::filesystem;

namespace kustavi {
auto kustavi_service::check_auth(const grpc::ServerContext *context) const
    -> bool {
  if (auth_token_.empty()) {
    return true;
  }
  const auto &metadata = context->client_metadata();
  const auto it = metadata.find(std::string(k_auth_token_header));
  return it != metadata.end() && it->second == auth_token_;
}

auto kustavi_service::require_session() -> std::optional<grpc::Status> {
  if (!has_active_session_) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "no active session; call ScanFolder first");
  }
  return std::nullopt;
}

} // namespace kustavi
