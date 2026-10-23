#include "core/grpc_kustavi_client.h"

#include <utility>

namespace kustavi::gui {

namespace {

auto make_stub(std::shared_ptr<grpc::Channel> const &channel) -> std::shared_ptr<Kustavi::Stub> {
  return std::shared_ptr<Kustavi::Stub>{Kustavi::NewStub(channel)};
}

} // namespace

auto connect_to_backend(int port) -> std::shared_ptr<grpc::Channel> {
  grpc::ChannelArguments arguments;
  arguments.SetMaxReceiveMessageSize(-1);
  arguments.SetMaxSendMessageSize(-1);
  return grpc::CreateCustomChannel("127.0.0.1:" + std::to_string(port),
                                   grpc::InsecureChannelCredentials(), arguments);
}

auto probe_backend(std::shared_ptr<grpc::Channel> const &channel, std::string const &token,
                   std::chrono::milliseconds timeout) -> call_result<GetInfoResponse> {
  // The probe is the one call with a deadline, so it uses its own context.
  auto stub = make_stub(channel);
  grpc::ClientContext context;
  context.AddMetadata(auth_token_header, token);
  context.set_deadline(std::chrono::system_clock::now() + timeout);
  GetInfoResponse response;
  auto const status = stub->GetInfo(&context, GetInfoRequest{}, &response);
  if (!status.ok()) {
    return std::unexpected(map_grpc_status(status));
  }
  return response;
}

grpc_kustavi_client::grpc_kustavi_client(std::shared_ptr<grpc::Channel> channel, std::string token)
    : channel_(std::move(channel)), stub_(make_stub(channel_)), token_(std::move(token)) {}

template <class Response, class Request, class Call>
auto grpc_kustavi_client::unary(Request const &request, Call call) -> call_result<Response> {
  grpc::ClientContext context;
  context.AddMetadata(auth_token_header, token_);
  Response response;
  auto const status = (stub_.get()->*call)(&context, request, &response);
  if (!status.ok()) {
    return std::unexpected(map_grpc_status(status));
  }
  return response;
}

template <class Event, class Request, class Start>
auto grpc_kustavi_client::stream(Request request, stream_handlers<Event> handlers, Start start)
    -> stream_subscription {
  auto control = std::make_shared<stream_subscription::control>();
  std::jthread worker{[stub = stub_, token = token_, control, request = std::move(request),
                       handlers = std::move(handlers), start] {
    grpc::ClientContext context;
    context.AddMetadata(auth_token_header, token);
    {
      std::scoped_lock lock{control->mutex};
      if (control->cancelled) {
        return;
      }
      control->cancel_call = [&context] { context.TryCancel(); };
    }
    auto reader = (stub.get()->*start)(&context, request);
    Event event;
    while (reader->Read(&event)) {
      if (control->is_cancelled()) {
        break;
      }
      if (handlers.on_event) {
        handlers.on_event(event);
      }
    }
    auto const status = reader->Finish();
    {
      // The context ends with this function, so the cancel function must not outlive it.
      std::scoped_lock lock{control->mutex};
      control->cancel_call = nullptr;
    }
    if (!control->is_cancelled() && handlers.on_done) {
      handlers.on_done(status.ok() ? std::nullopt
                                   : std::optional<backend_error>{map_grpc_status(status)});
    }
  }};
  return {std::move(control), std::move(worker)};
}

namespace {

auto expect_ok(call_result<ShutdownResponse> result) -> call_result<void> {
  if (!result) {
    return std::unexpected(std::move(result.error()));
  }
  return {};
}

auto expect_saved(call_result<SaveSessionStateResponse> result) -> call_result<void> {
  if (!result) {
    return std::unexpected(std::move(result.error()));
  }
  return {};
}

} // namespace

auto grpc_kustavi_client::get_info() -> call_result<GetInfoResponse> {
  return unary<GetInfoResponse>(GetInfoRequest{}, &Kustavi::Stub::GetInfo);
}

auto grpc_kustavi_client::shutdown() -> call_result<void> {
  return expect_ok(unary<ShutdownResponse>(ShutdownRequest{}, &Kustavi::Stub::Shutdown));
}

auto grpc_kustavi_client::inspect_session(std::string folder)
    -> call_result<InspectSessionResponse> {
  InspectSessionRequest request;
  request.set_folder(std::move(folder));
  return unary<InspectSessionResponse>(request, &Kustavi::Stub::InspectSession);
}

auto grpc_kustavi_client::get_session_results() -> call_result<GetSessionResultsResponse> {
  return unary<GetSessionResultsResponse>(GetSessionResultsRequest{},
                                          &Kustavi::Stub::GetSessionResults);
}

auto grpc_kustavi_client::save_session_state(SaveSessionStateRequest request) -> call_result<void> {
  return expect_saved(
      unary<SaveSessionStateResponse>(request, &Kustavi::Stub::SaveSessionState));
}

auto grpc_kustavi_client::preview_quality_thresholds(PreviewQualityThresholdsRequest request)
    -> call_result<PreviewQualityThresholdsResponse> {
  return unary<PreviewQualityThresholdsResponse>(request,
                                                 &Kustavi::Stub::PreviewQualityThresholds);
}

auto grpc_kustavi_client::estimate_commit(CommitRequest request)
    -> call_result<EstimateCommitResponse> {
  return unary<EstimateCommitResponse>(request, &Kustavi::Stub::EstimateCommit);
}

auto grpc_kustavi_client::scan_folder(ScanFolderRequest request,
                                      stream_handlers<ScanEvent> handlers) -> stream_subscription {
  return stream<ScanEvent>(std::move(request), std::move(handlers), &Kustavi::Stub::ScanFolder);
}

auto grpc_kustavi_client::run_quality_pass(RunQualityPassRequest request,
                                           stream_handlers<QualityEvent> handlers)
    -> stream_subscription {
  return stream<QualityEvent>(std::move(request), std::move(handlers),
                              &Kustavi::Stub::RunQualityPass);
}

auto grpc_kustavi_client::ensure_model(stream_handlers<ModelEvent> handlers)
    -> stream_subscription {
  return stream<ModelEvent>(EnsureModelRequest{}, std::move(handlers),
                            &Kustavi::Stub::EnsureModel);
}

auto grpc_kustavi_client::run_junk_pass(RunJunkPassRequest request,
                                        stream_handlers<JunkEvent> handlers)
    -> stream_subscription {
  return stream<JunkEvent>(std::move(request), std::move(handlers), &Kustavi::Stub::RunJunkPass);
}

auto grpc_kustavi_client::run_similar_pass(RunSimilarPassRequest request,
                                           stream_handlers<SimilarEvent> handlers)
    -> stream_subscription {
  return stream<SimilarEvent>(std::move(request), std::move(handlers),
                              &Kustavi::Stub::RunSimilarPass);
}

auto grpc_kustavi_client::run_trips_pass(RunTripsPassRequest request,
                                         stream_handlers<TripsEvent> handlers)
    -> stream_subscription {
  return stream<TripsEvent>(std::move(request), std::move(handlers),
                            &Kustavi::Stub::RunTripsPass);
}

auto grpc_kustavi_client::run_video_pass(RunVideoPassRequest request,
                                         stream_handlers<VideoEvent> handlers)
    -> stream_subscription {
  return stream<VideoEvent>(std::move(request), std::move(handlers),
                            &Kustavi::Stub::RunVideoPass);
}

auto grpc_kustavi_client::commit(CommitRequest request, stream_handlers<CommitEvent> handlers)
    -> stream_subscription {
  return stream<CommitEvent>(std::move(request), std::move(handlers), &Kustavi::Stub::Commit);
}

} // namespace kustavi::gui
