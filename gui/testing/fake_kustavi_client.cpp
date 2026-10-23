#include "testing/fake_kustavi_client.h"

#include <utility>

namespace kustavi::gui::testing {

template <class T> auto fake_kustavi_client::unary_result(T const &response) -> call_result<T> {
  if (unary_error) {
    return std::unexpected(*unary_error);
  }
  return response;
}

template <class Event>
auto fake_kustavi_client::play(std::vector<Event> const &events, stream_handlers<Event> handlers)
    -> stream_subscription {
  auto control = std::make_shared<stream_subscription::control>();
  auto deliver = [control, events, handlers = std::move(handlers), error = stream_error] {
    for (auto const &event : events) {
      if (control->is_cancelled()) {
        return;
      }
      if (handlers.on_event) {
        handlers.on_event(event);
      }
    }
    if (!control->is_cancelled() && handlers.on_done) {
      handlers.on_done(error);
    }
  };
  if (hold_streams) {
    held_.push_back({control, std::move(deliver)});
  } else {
    deliver();
  }
  return {std::move(control), std::jthread{}};
}

auto fake_kustavi_client::release_streams() -> void {
  auto held = std::exchange(held_, {});
  for (auto &stream : held) {
    stream.deliver();
  }
}

auto fake_kustavi_client::get_info() -> call_result<GetInfoResponse> {
  return unary_result(info);
}

auto fake_kustavi_client::shutdown() -> call_result<void> {
  ++shutdown_calls;
  if (unary_error) {
    return std::unexpected(*unary_error);
  }
  return {};
}

auto fake_kustavi_client::inspect_session(std::string folder)
    -> call_result<InspectSessionResponse> {
  last_inspected_folder = std::move(folder);
  return unary_result(inspect_response);
}

auto fake_kustavi_client::get_session_results() -> call_result<GetSessionResultsResponse> {
  return unary_result(session_results);
}

auto fake_kustavi_client::save_session_state(SaveSessionStateRequest request) -> call_result<void> {
  saved_session_states.push_back(std::move(request));
  if (unary_error) {
    return std::unexpected(*unary_error);
  }
  return {};
}

auto fake_kustavi_client::preview_quality_thresholds(PreviewQualityThresholdsRequest request)
    -> call_result<PreviewQualityThresholdsResponse> {
  last_preview_request = std::move(request);
  return unary_result(preview_response);
}

auto fake_kustavi_client::estimate_commit(CommitRequest request)
    -> call_result<EstimateCommitResponse> {
  last_commit_request = std::move(request);
  return unary_result(estimate_response);
}

auto fake_kustavi_client::scan_folder(ScanFolderRequest request,
                                      stream_handlers<ScanEvent> handlers) -> stream_subscription {
  last_scan_request = std::move(request);
  return play(scan_events, std::move(handlers));
}

auto fake_kustavi_client::run_quality_pass(RunQualityPassRequest request,
                                           stream_handlers<QualityEvent> handlers)
    -> stream_subscription {
  last_quality_request = std::move(request);
  return play(quality_events, std::move(handlers));
}

auto fake_kustavi_client::ensure_model(stream_handlers<ModelEvent> handlers)
    -> stream_subscription {
  ++ensure_model_calls;
  return play(model_events, std::move(handlers));
}

auto fake_kustavi_client::run_junk_pass(RunJunkPassRequest request,
                                        stream_handlers<JunkEvent> handlers)
    -> stream_subscription {
  last_junk_request = std::move(request);
  return play(junk_events, std::move(handlers));
}

auto fake_kustavi_client::run_similar_pass(RunSimilarPassRequest request,
                                           stream_handlers<SimilarEvent> handlers)
    -> stream_subscription {
  last_similar_request = std::move(request);
  return play(similar_events, std::move(handlers));
}

auto fake_kustavi_client::run_trips_pass(RunTripsPassRequest request,
                                         stream_handlers<TripsEvent> handlers)
    -> stream_subscription {
  last_trips_request = std::move(request);
  return play(trips_events, std::move(handlers));
}

auto fake_kustavi_client::run_video_pass(RunVideoPassRequest request,
                                         stream_handlers<VideoEvent> handlers)
    -> stream_subscription {
  last_video_request = std::move(request);
  return play(video_events, std::move(handlers));
}

auto fake_kustavi_client::commit(CommitRequest request, stream_handlers<CommitEvent> handlers)
    -> stream_subscription {
  last_commit_request = std::move(request);
  return play(commit_events, std::move(handlers));
}

} // namespace kustavi::gui::testing
