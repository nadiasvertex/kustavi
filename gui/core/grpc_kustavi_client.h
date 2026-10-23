#pragma once

#include "core/kustavi_client.h"

#include <grpcpp/grpcpp.h>

#include "proto/service.grpc.pb.h"

#include <chrono>
#include <memory>
#include <string>

namespace kustavi::gui {

// The gRPC metadata header that carries the auth token.
inline constexpr auto auth_token_header = "x-kustavi-auth-token";

// A channel to the back end on the loopback address. Messages may be as large as the back end
// sends, because a resumed session returns every stored result in one reply.
[[nodiscard]] auto connect_to_backend(int port) -> std::shared_ptr<grpc::Channel>;

// Asks the back end for its version, to see that it answers before the GUI uses it.
[[nodiscard]] auto probe_backend(std::shared_ptr<grpc::Channel> const &channel,
                                 std::string const &token, std::chrono::milliseconds timeout)
    -> call_result<GetInfoResponse>;

// The production client: gRPC over `channel`, with the auth token on every call.
class grpc_kustavi_client final : public kustavi_client {
public:
  grpc_kustavi_client(std::shared_ptr<grpc::Channel> channel, std::string token);

  [[nodiscard]] auto get_info() -> call_result<GetInfoResponse> override;
  [[nodiscard]] auto shutdown() -> call_result<void> override;
  [[nodiscard]] auto inspect_session(std::string folder)
      -> call_result<InspectSessionResponse> override;
  [[nodiscard]] auto get_session_results() -> call_result<GetSessionResultsResponse> override;
  [[nodiscard]] auto save_session_state(SaveSessionStateRequest request)
      -> call_result<void> override;
  [[nodiscard]] auto preview_quality_thresholds(PreviewQualityThresholdsRequest request)
      -> call_result<PreviewQualityThresholdsResponse> override;
  [[nodiscard]] auto estimate_commit(CommitRequest request)
      -> call_result<EstimateCommitResponse> override;

  [[nodiscard]] auto scan_folder(ScanFolderRequest request, stream_handlers<ScanEvent> handlers)
      -> stream_subscription override;
  [[nodiscard]] auto run_quality_pass(RunQualityPassRequest request,
                                      stream_handlers<QualityEvent> handlers)
      -> stream_subscription override;
  [[nodiscard]] auto ensure_model(stream_handlers<ModelEvent> handlers)
      -> stream_subscription override;
  [[nodiscard]] auto run_junk_pass(RunJunkPassRequest request, stream_handlers<JunkEvent> handlers)
      -> stream_subscription override;
  [[nodiscard]] auto run_similar_pass(RunSimilarPassRequest request,
                                      stream_handlers<SimilarEvent> handlers)
      -> stream_subscription override;
  [[nodiscard]] auto run_trips_pass(RunTripsPassRequest request,
                                    stream_handlers<TripsEvent> handlers)
      -> stream_subscription override;
  [[nodiscard]] auto run_video_pass(RunVideoPassRequest request,
                                    stream_handlers<VideoEvent> handlers)
      -> stream_subscription override;
  [[nodiscard]] auto commit(CommitRequest request, stream_handlers<CommitEvent> handlers)
      -> stream_subscription override;

private:
  template <class Response, class Request, class Call>
  [[nodiscard]] auto unary(Request const &request, Call call) -> call_result<Response>;
  template <class Event, class Request, class Start>
  [[nodiscard]] auto stream(Request request, stream_handlers<Event> handlers, Start start)
      -> stream_subscription;

  std::shared_ptr<grpc::Channel> channel_;
  std::shared_ptr<Kustavi::Stub> stub_;
  std::string token_;
};

} // namespace kustavi::gui
