#include "kustavi_service.h"

#include "paths.h"
#include "version.h"

#include <spdlog/spdlog.h>

#include <utility>

namespace kustavi {

kustavi_service::kustavi_service(std::string auth_token)
    : auth_token_(std::move(auth_token)) {
  if (auth_token_.empty()) {
    spdlog::warn("no --token provided; gRPC auth validation is disabled");
  }
}

auto kustavi_service::try_begin_pass() -> std::optional<grpc::Status> {
  bool expected = false;
  if (!pass_active_.compare_exchange_strong(expected, true)) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "another pass is already running");
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

auto kustavi_service::GetInfo(grpc::ServerContext *context,
                              const GetInfoRequest *request,
                              GetInfoResponse *response) -> grpc::Status {
  (void)request;
  if (!check_auth(context)) {
    return unauthenticated();
  }
  response->set_version(std::string(kustavi::version));
  for (const auto ext : image::supported_image_extensions) {
    response->add_supported_formats(std::string(ext));
  }
  response->set_model_name(std::string(k_model_name));
  return grpc::Status::OK;
}

auto kustavi_service::Shutdown(grpc::ServerContext *context,
                               const ShutdownRequest *request,
                               ShutdownResponse *response) -> grpc::Status {
  (void)request;
  (void)response;
  if (!check_auth(context)) {
    return unauthenticated();
  }
  // Idempotent: only the first request flips the flag. serve() observes it
  // and stops the server off the worker pool (see is_shutdown_requested).
  if (shutdown_requested_.exchange(true) == false) {
    spdlog::info("shutdown requested via gRPC");
  }
  return grpc::Status::OK;
}
} // namespace kustavi
