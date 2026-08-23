#include "kustavi_service.h"

#include "algorithm/append_range.h"
#include "paths.h"

#include <spdlog/spdlog.h>

namespace kustavi {

using algorithm::append_range;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Pass 3: model + junk (stubs until the vision pipeline lands)
// ---------------------------------------------------------------------------

auto kustavi_service::EnsureModel(grpc::ServerContext *context,
                                  const EnsureModelRequest *request,
                                  grpc::ServerWriter<ModelEvent> *writer)
    -> grpc::Status {
  (void)request;
  (void)writer;
  if (!check_auth(context)) {
    return unauthenticated();
  }
  return {grpc::StatusCode::UNIMPLEMENTED,
          "vision model pipeline is not implemented yet"};
}

auto kustavi_service::RunJunkPass(grpc::ServerContext *context,
                                  const RunJunkPassRequest *request,
                                  grpc::ServerWriter<JunkEvent> *writer)
    -> grpc::Status {
  (void)request;
  (void)writer;
  if (!check_auth(context)) {
    return unauthenticated();
  }
  return {grpc::StatusCode::UNIMPLEMENTED,
          "vision model pipeline is not implemented yet"};
}
} // namespace kustavi
