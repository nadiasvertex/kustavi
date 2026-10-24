#include "core/engine_client.h"

#include <exception>
#include <utility>

namespace kustavi::gui {

namespace {

auto failure_of(std::exception const &error) -> backend_error {
  return {status_code::internal, error.what()};
}

} // namespace

template <class Response, class Request, class Call>
auto engine_client::unary(Request const &request, Call call) -> call_result<Response> {
  Response response;
  try {
    auto const result = (engine_.get()->*call)(&request, &response);
    if (!result.ok()) {
      return std::unexpected(to_backend_error(result));
    }
  } catch (std::exception const &error) {
    return std::unexpected(failure_of(error));
  }
  return response;
}

template <class Event, class Request, class Call>
auto engine_client::stream(Request request, stream_handlers<Event> handlers, Call call)
    -> stream_subscription {
  auto control = std::make_shared<stream_subscription::control>();
  std::jthread worker{[backend = engine_, control, request = std::move(request),
                       handlers = std::move(handlers), call] {
    request_context context;
    {
      std::scoped_lock lock{control->mutex};
      if (control->cancelled) {
        return;
      }
      control->cancel_call = [&context] { context.cancel(); };
    }
    // The pass hands each event to the writer on this thread. Once the stream is cancelled the
    // writer refuses events, which stops the pass.
    event_writer<Event> writer{[&](Event const &event) {
      if (control->is_cancelled()) {
        return false;
      }
      if (handlers.on_event) {
        handlers.on_event(event);
      }
      return true;
    }};
    std::optional<backend_error> failure;
    try {
      auto const result = (backend.get()->*call)(&context, &request, &writer);
      if (!result.ok()) {
        failure = to_backend_error(result);
      }
    } catch (std::exception const &error) {
      failure = failure_of(error);
    }
    {
      // The context ends with this function, so the cancel function must not outlive it.
      std::scoped_lock lock{control->mutex};
      control->cancel_call = nullptr;
    }
    if (!control->is_cancelled() && handlers.on_done) {
      handlers.on_done(std::move(failure));
    }
  }};
  return {std::move(control), std::move(worker)};
}

auto engine_client::get_info() -> call_result<GetInfoResponse> {
  return unary<GetInfoResponse>(GetInfoRequest{}, &engine::get_info);
}

auto engine_client::inspect_session(std::string folder) -> call_result<InspectSessionResponse> {
  InspectSessionRequest request;
  request.set_folder(std::move(folder));
  return unary<InspectSessionResponse>(request, &engine::inspect_session);
}

auto engine_client::get_session_results() -> call_result<GetSessionResultsResponse> {
  return unary<GetSessionResultsResponse>(GetSessionResultsRequest{}, &engine::get_session_results);
}

auto engine_client::save_session_state(SaveSessionStateRequest request) -> call_result<void> {
  auto saved = unary<SaveSessionStateResponse>(request, &engine::save_session_state);
  if (!saved) {
    return std::unexpected(std::move(saved.error()));
  }
  return {};
}

auto engine_client::preview_quality_thresholds(PreviewQualityThresholdsRequest request)
    -> call_result<PreviewQualityThresholdsResponse> {
  return unary<PreviewQualityThresholdsResponse>(request, &engine::preview_quality_thresholds);
}

auto engine_client::estimate_commit(CommitRequest request) -> call_result<EstimateCommitResponse> {
  return unary<EstimateCommitResponse>(request, &engine::estimate_commit);
}

auto engine_client::scan_folder(ScanFolderRequest request, stream_handlers<ScanEvent> handlers)
    -> stream_subscription {
  return stream<ScanEvent>(std::move(request), std::move(handlers), &engine::scan_folder);
}

auto engine_client::run_quality_pass(RunQualityPassRequest request,
                                     stream_handlers<QualityEvent> handlers)
    -> stream_subscription {
  return stream<QualityEvent>(std::move(request), std::move(handlers), &engine::run_quality_pass);
}

auto engine_client::ensure_model(stream_handlers<ModelEvent> handlers) -> stream_subscription {
  return stream<ModelEvent>(EnsureModelRequest{}, std::move(handlers), &engine::ensure_model);
}

auto engine_client::run_junk_pass(RunJunkPassRequest request, stream_handlers<JunkEvent> handlers)
    -> stream_subscription {
  return stream<JunkEvent>(std::move(request), std::move(handlers), &engine::run_junk_pass);
}

auto engine_client::run_similar_pass(RunSimilarPassRequest request,
                                     stream_handlers<SimilarEvent> handlers)
    -> stream_subscription {
  return stream<SimilarEvent>(std::move(request), std::move(handlers), &engine::run_similar_pass);
}

auto engine_client::run_trips_pass(RunTripsPassRequest request,
                                   stream_handlers<TripsEvent> handlers) -> stream_subscription {
  return stream<TripsEvent>(std::move(request), std::move(handlers), &engine::run_trips_pass);
}

auto engine_client::run_video_pass(RunVideoPassRequest request,
                                   stream_handlers<VideoEvent> handlers) -> stream_subscription {
  return stream<VideoEvent>(std::move(request), std::move(handlers), &engine::run_video_pass);
}

auto engine_client::commit(CommitRequest request, stream_handlers<CommitEvent> handlers)
    -> stream_subscription {
  return stream<CommitEvent>(std::move(request), std::move(handlers), &engine::commit);
}

} // namespace kustavi::gui
