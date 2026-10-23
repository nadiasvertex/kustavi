#pragma once

#include "core/backend_error.h"

#include "proto/service.pb.h"

#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace kustavi::gui {

template <class T> using call_result = std::expected<T, backend_error>;

// What a streaming call reports. Both functions run on the worker thread that reads the stream,
// so they post to the `ui_queue` and do not touch widgets or app state.
template <class Event> struct stream_handlers {
  // Called for each event, in order.
  std::function<void(Event const &)> on_event;
  // Called once after the last event: with nothing when the stream ended normally, and with the
  // error when it ended in a failure. It is not called after `cancel`.
  std::function<void(std::optional<backend_error>)> on_done;
};

// A running streaming call. Cancelling it, or destroying it, ends the call in the back end, which
// stops the pass. After `cancel` returns no handler runs again, except one that is already
// running.
class stream_subscription {
public:
  // What a stream needs from its call: a way to cancel it, shared with the worker.
  struct control {
    std::mutex mutex;
    bool cancelled = false;
    // Cancels the call in flight. Set while the call runs.
    std::function<void()> cancel_call;

    auto cancel() -> void;
    [[nodiscard]] auto is_cancelled() -> bool;
  };

  stream_subscription() = default;
  stream_subscription(std::shared_ptr<control> state, std::jthread worker)
      : control_(std::move(state)), worker_(std::move(worker)) {}
  stream_subscription(stream_subscription &&) noexcept = default;
  auto operator=(stream_subscription &&other) noexcept -> stream_subscription &;
  stream_subscription(stream_subscription const &) = delete;
  auto operator=(stream_subscription const &) -> stream_subscription & = delete;
  ~stream_subscription() { cancel(); }

  auto cancel() -> void;
  [[nodiscard]] auto active() const -> bool { return control_ != nullptr; }

private:
  std::shared_ptr<control> control_;
  std::jthread worker_;
};

// The GUI's one way to the back end. Screens and the controller never use the generated stubs. The
// unary calls block, so call them from a worker. The streaming calls start a worker themselves.
class kustavi_client {
public:
  virtual ~kustavi_client() = default;

  [[nodiscard]] virtual auto get_info() -> call_result<GetInfoResponse> = 0;
  [[nodiscard]] virtual auto shutdown() -> call_result<void> = 0;

  // Reads whether the folder holds a session that can resume. Changes nothing.
  [[nodiscard]] virtual auto inspect_session(std::string folder)
      -> call_result<InspectSessionResponse> = 0;
  // Every stored pass result and the user's decisions, after a resume scan.
  [[nodiscard]] virtual auto get_session_results() -> call_result<GetSessionResultsResponse> = 0;
  [[nodiscard]] virtual auto save_session_state(SaveSessionStateRequest request)
      -> call_result<void> = 0;
  // Counts what the stored metrics flag at the thresholds, without analyzing anything.
  [[nodiscard]] virtual auto preview_quality_thresholds(PreviewQualityThresholdsRequest request)
      -> call_result<PreviewQualityThresholdsResponse> = 0;
  [[nodiscard]] virtual auto estimate_commit(CommitRequest request)
      -> call_result<EstimateCommitResponse> = 0;

  [[nodiscard]] virtual auto scan_folder(ScanFolderRequest request,
                                         stream_handlers<ScanEvent> handlers)
      -> stream_subscription = 0;
  [[nodiscard]] virtual auto run_quality_pass(RunQualityPassRequest request,
                                              stream_handlers<QualityEvent> handlers)
      -> stream_subscription = 0;
  [[nodiscard]] virtual auto ensure_model(stream_handlers<ModelEvent> handlers)
      -> stream_subscription = 0;
  [[nodiscard]] virtual auto run_junk_pass(RunJunkPassRequest request,
                                           stream_handlers<JunkEvent> handlers)
      -> stream_subscription = 0;
  [[nodiscard]] virtual auto run_similar_pass(RunSimilarPassRequest request,
                                              stream_handlers<SimilarEvent> handlers)
      -> stream_subscription = 0;
  [[nodiscard]] virtual auto run_trips_pass(RunTripsPassRequest request,
                                            stream_handlers<TripsEvent> handlers)
      -> stream_subscription = 0;
  [[nodiscard]] virtual auto run_video_pass(RunVideoPassRequest request,
                                            stream_handlers<VideoEvent> handlers)
      -> stream_subscription = 0;
  [[nodiscard]] virtual auto commit(CommitRequest request, stream_handlers<CommitEvent> handlers)
      -> stream_subscription = 0;
};

} // namespace kustavi::gui
