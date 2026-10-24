#pragma once

#include "core/kustavi_client.h"

#include "engine.h"

#include <memory>

namespace kustavi::gui {

// The production client: calls the engine in this process. A streaming call runs its pass on a
// worker thread of its own, and cancelling it cancels the pass.
class engine_client final : public kustavi_client {
public:
  explicit engine_client(std::shared_ptr<engine> backend) : engine_(std::move(backend)) {}

  [[nodiscard]] auto get_info() -> call_result<GetInfoResponse> override;
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
  // Runs the unary call `call` of the engine and returns the reply or the failure.
  template <class Response, class Request, class Call>
  auto unary(Request const &request, Call call) -> call_result<Response>;

  // Starts a worker that runs the streaming call `call` of the engine and reports to `handlers`.
  template <class Event, class Request, class Call>
  auto stream(Request request, stream_handlers<Event> handlers, Call call) -> stream_subscription;

  std::shared_ptr<engine> engine_;
};

} // namespace kustavi::gui
