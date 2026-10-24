#include "engine.h"

#include "paths.h"
#include "version.h"

#include <spdlog/spdlog.h>

#include <utility>

namespace kustavi {

auto engine::try_begin_pass() -> std::optional<status> {
  bool expected = false;
  if (!pass_active_.compare_exchange_strong(expected, true)) {
    return status(status_code::failed_precondition,
                  "another pass is already running");
  }
  return std::nullopt;
}

auto engine::require_session() -> std::optional<status> {
  if (!has_active_session_) {
    return status(status_code::failed_precondition,
                  "no active session; call scan_folder first");
  }
  return std::nullopt;
}

auto engine::get_info(const GetInfoRequest *request, GetInfoResponse *response)
    -> status {
  (void)request;
  response->set_version(std::string(kustavi::version));
  for (const auto ext : image::supported_image_extensions) {
    response->add_supported_formats(std::string(ext));
  }
  for (const auto ext : image::supported_video_extensions) {
    response->add_supported_formats(std::string(ext));
  }
  response->set_model_name(std::string(k_model_name));
  return ok_status();
}

} // namespace kustavi
