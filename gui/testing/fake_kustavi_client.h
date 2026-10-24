#pragma once

#include "core/kustavi_client.h"

#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace kustavi::gui::testing {

// A `kustavi_client` that plays back what a test scripts and records what it was asked. Streams
// deliver their events at once, on the calling thread, before the call returns, so a test that
// drains a `ui_queue` sees them in a fixed order. With `hold_streams` set, a stream delivers
// nothing until `release_streams`, which lets a test cancel one first.
class fake_kustavi_client final : public kustavi_client {
public:
  // What the unary calls return. A call that has an error returns it instead.
  GetInfoResponse info;
  InspectSessionResponse inspect_response;
  GetSessionResultsResponse session_results;
  PreviewQualityThresholdsResponse preview_response;
  EstimateCommitResponse estimate_response;
  std::optional<backend_error> unary_error;

  // The events of each stream, and the failure that ends it. A stream with an error ends in
  // `on_done(error)` after its events.
  std::vector<ScanEvent> scan_events;
  std::vector<QualityEvent> quality_events;
  std::vector<ModelEvent> model_events;
  std::vector<JunkEvent> junk_events;
  std::vector<SimilarEvent> similar_events;
  std::vector<TripsEvent> trips_events;
  std::vector<VideoEvent> video_events;
  std::vector<CommitEvent> commit_events;
  std::optional<backend_error> stream_error;

  bool hold_streams = false;

  // What the client was asked, newest last.
  std::string last_inspected_folder;
  std::optional<ScanFolderRequest> last_scan_request;
  std::optional<RunQualityPassRequest> last_quality_request;
  std::optional<RunJunkPassRequest> last_junk_request;
  std::optional<RunSimilarPassRequest> last_similar_request;
  std::optional<RunTripsPassRequest> last_trips_request;
  std::optional<RunVideoPassRequest> last_video_request;
  std::optional<CommitRequest> last_commit_request;
  std::optional<CommitRequest> last_estimate_request;
  std::optional<PreviewQualityThresholdsRequest> last_preview_request;
  std::vector<SaveSessionStateRequest> saved_session_states;
  int ensure_model_calls = 0;

  // Delivers the events of every stream that was held. A cancelled stream delivers nothing.
  auto release_streams() -> void;
  [[nodiscard]] auto held_streams() const -> std::size_t { return held_.size(); }

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
  template <class T> [[nodiscard]] auto unary_result(T const &response) -> call_result<T>;
  template <class Event>
  [[nodiscard]] auto play(std::vector<Event> const &events, stream_handlers<Event> handlers)
      -> stream_subscription;

  struct held_stream {
    std::shared_ptr<stream_subscription::control> control;
    std::function<void()> deliver;
  };
  std::deque<held_stream> held_;
};

} // namespace kustavi::gui::testing
