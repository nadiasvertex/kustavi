#pragma once

#include "collection/event_queue.h"
#include "pass/commit.h"
#include "pass/downscaler.h"
#include "pass/quality.h"
#include "pass/repair.h"
#include "pass/trips.h"
#include "store/database.h"
#include "store/store.h"

#include "status.h"

#include <proto/service.pb.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace kustavi {

/// Vision model reported by `GetInfo` (junk pass).
inline constexpr std::string_view k_model_name = "qwen2.5-vl-3b";

// --- per-pass streaming events --------------------------------------------
// Carried by event_queue between the pass worker and the thread that writes
// events out.

struct scan_progress_evt {
  std::size_t files_seen = 0;
  std::size_t images_found = 0;
};
struct scan_image_evt {
  image::ingestion_result result;
};
// Re-emitted from the saved image index on a resume scan (no ingestion).
struct scan_meta_evt {
  store::image_record record;
};
struct scan_complete_evt {
  std::size_t images = 0;
  std::size_t skipped_files = 0;
  std::vector<std::string> errors;
  bool resumed = false;
  int resume_step = 0;
};
using scan_event = std::variant<scan_progress_evt, scan_image_evt,
                                scan_meta_evt, scan_complete_evt>;

struct quality_progress_evt {
  std::size_t done = 0;
  std::size_t total = 0;
};
struct quality_flag_evt {
  std::string image_id;
  std::vector<QualityReason> reasons;
  double sharpness = 0.0;
  double exposure_score = 0.0;
};
struct quality_complete_evt {
  std::size_t flagged = 0;
  std::size_t total = 0;
};
using quality_event =
    std::variant<quality_progress_evt, quality_flag_evt, quality_complete_evt>;

struct model_progress_evt {
  std::uint64_t done_bytes = 0;
  std::uint64_t total_bytes = 0;
  double speed_bps = 0.0;
};
struct model_ready_evt {
  std::string model_name;
  std::uint64_t size_bytes = 0;
};
using model_event = std::variant<model_progress_evt, model_ready_evt>;

struct junk_progress_evt {
  std::size_t done = 0;
  std::size_t total = 0;
};
struct junk_flag_evt {
  std::string image_id;
  std::string reason;
  double confidence = 0.0;
};
struct junk_complete_evt {
  std::size_t flagged = 0;
  std::size_t total = 0;
};
using junk_event =
    std::variant<junk_progress_evt, junk_flag_evt, junk_complete_evt>;

struct similar_progress_evt {
  std::size_t done = 0;
  std::size_t total = 0;
};
struct similar_group_evt {
  std::vector<std::string> image_ids;
  std::string keeper_id;
  std::vector<double> member_scores;
};
struct similar_complete_evt {
  std::size_t groups = 0;
  std::size_t total_images = 0;
};
using similar_event =
    std::variant<similar_progress_evt, similar_group_evt, similar_complete_evt>;

struct trips_progress_evt {
  std::size_t done = 0;
  std::size_t total = 0;
};
struct trips_result_evt {
  trip value;
};
struct trips_complete_evt {
  std::size_t trips = 0;
  std::size_t unassigned = 0;
};
using trips_event =
    std::variant<trips_progress_evt, trips_result_evt, trips_complete_evt>;

struct video_progress_evt {
  std::size_t done = 0;
  std::size_t total = 0;
};
struct video_flag_evt {
  std::string video_id;
  std::string reason;
  double confidence = 0.0;
};
struct video_complete_evt {
  std::size_t flagged = 0;
  std::size_t total = 0;
};
using video_event =
    std::variant<video_progress_evt, video_flag_evt, video_complete_evt>;

struct repair_progress_evt {
  std::size_t done = 0;
  std::size_t total = 0;
};
struct repair_item_evt {
  repaired_metadata value;
};
struct repair_offset_evt {
  clock_offset_suggestion value;
  bool applied = false;
};
struct repair_complete_evt {
  std::size_t dates_from_filename = 0;
  std::size_t dates_from_modified_time = 0;
  std::size_t dates_shifted = 0;
  std::size_t gps_filled = 0;
};
using repair_event = std::variant<repair_progress_evt, repair_item_evt,
                                  repair_offset_evt, repair_complete_evt>;

struct commit_progress_evt {
  std::size_t done = 0;
  std::size_t total = 0;
  std::string current_name;
};
struct commit_complete_evt {
  std::size_t copied = 0;
  std::size_t skipped = 0;
  std::size_t already_present = 0;
  std::size_t companions = 0;
  std::vector<std::string> errors;
};
using commit_event = std::variant<commit_progress_evt, commit_complete_evt>;

// --- pass lifecycle helpers --------------------------------------------------

/** Tracks acquisition of the single-pass lock and releases it on scope exit. */
class pass_guard {
public:
  pass_guard(std::atomic<bool> &flag, bool acquired)
      : flag_(flag), acquired_(acquired) {}
  ~pass_guard() {
    if (acquired_) {
      flag_.store(false);
    }
  }
  pass_guard(const pass_guard &) = delete;
  pass_guard &operator=(const pass_guard &) = delete;

private:
  std::atomic<bool> &flag_;
  bool acquired_;
};

/** Starts the pass worker: runs `fn` (which receives the shared stop token)
 * on a background thread, catches any exception into `error`, and always
 * closes `queue` when the worker exits. */
template <class QueueEvent, class Fn>
auto run_producer(event_queue<QueueEvent> &queue, std::stop_source &stop_source,
                  std::exception_ptr &error, Fn fn) -> std::thread {
  return std::thread([&queue, &stop_source, &error, fn = std::move(fn)] {
    try {
      fn(stop_source.get_token());
    } catch (...) {
      error = std::current_exception();
    }
    queue.close();
  });
}

/** Drains the pass event queue into the event writer.
 *
 * Checks for cancellation on a 200 ms cadence; on cancellation or when the
 * writer refuses an event it stops the producer and closes the queue.
 * @return ok when the stream completed, cancelled when the caller went away.
 */
template <class ProtoEvent, class QueueEvent, class WriteFn>
auto stream_pass(request_context *ctx, event_writer<ProtoEvent> *writer,
                 event_queue<QueueEvent> &queue,
                 std::stop_source &producer_stop, WriteFn write_one) -> status {
  (void)writer;
  while (true) {
    const auto ev = queue.wait(std::chrono::milliseconds{200});
    if (ctx->is_cancelled()) {
      producer_stop.request_stop();
      queue.close();
      return status(status_code::cancelled, "");
    }
    if (ev.has_value()) {
      if (!write_one(*ev)) {
        producer_stop.request_stop();
        queue.close();
        return status(status_code::cancelled, "client disconnected");
      }
      continue;
    }
    if (queue.is_closed()) {
      return ok_status();
    }
  }
}

/** Converts a producer failure (if any) into an internal status. */
inline auto producer_error_status(const std::exception_ptr &error)
    -> std::optional<status> {
  if (error == nullptr) {
    return std::nullopt;
  }
  try {
    std::rethrow_exception(error);
  } catch (const std::exception &e) {
    return status(status_code::internal, e.what());
  } catch (...) {
    return status(status_code::internal, "unknown internal error");
  }
}

// --- the engine --------------------------------------------------------------

/** The back end: one session over one folder of photos, and the passes that
 * analyze it.
 *
 * Calls block the thread that makes them. A streaming call runs its pass and
 * hands each event to the writer on the calling thread, so a caller starts it
 * on a thread of its own. Another thread ends it by cancelling the request
 * context. One pass runs at a time; `ensure_model` is exempt, and may run
 * beside the scan. The unary calls may run beside a pass.
 */
class engine {
public:
  engine() = default;
  engine(const engine &) = delete;
  engine &operator=(const engine &) = delete;

  auto get_info(const GetInfoRequest *request, GetInfoResponse *response)
      -> status;

  /** Reads whether a folder holds a session that can resume. Changes nothing.
   */
  auto inspect_session(const InspectSessionRequest *request,
                       InspectSessionResponse *response) -> status;
  /** Counts what the stored quality metrics flag at the given thresholds. */
  auto
  preview_quality_thresholds(const PreviewQualityThresholdsRequest *request,
                             PreviewQualityThresholdsResponse *response)
      -> status;

  /** Every stored pass result and the user's decisions, after a resume scan. */
  auto get_session_results(const GetSessionResultsRequest *request,
                           GetSessionResultsResponse *response) -> status;
  /** Writes the wizard's step, tunables and decisions so a later launch can
   * resume. */
  auto save_session_state(const SaveSessionStateRequest *request,
                          SaveSessionStateResponse *response) -> status;
  /** Sizes a commit without writing anything. */
  auto estimate_commit(const CommitRequest *request,
                       EstimateCommitResponse *response) -> status;

  auto scan_folder(request_context *context, const ScanFolderRequest *request,
                   event_writer<ScanEvent> *writer) -> status;
  auto run_quality_pass(request_context *context,
                        const RunQualityPassRequest *request,
                        event_writer<QualityEvent> *writer) -> status;
  auto ensure_model(request_context *context, const EnsureModelRequest *request,
                    event_writer<ModelEvent> *writer) -> status;
  auto run_junk_pass(request_context *context,
                     const RunJunkPassRequest *request,
                     event_writer<JunkEvent> *writer) -> status;
  auto run_similar_pass(request_context *context,
                        const RunSimilarPassRequest *request,
                        event_writer<SimilarEvent> *writer) -> status;
  auto run_trips_pass(request_context *context,
                      const RunTripsPassRequest *request,
                      event_writer<TripsEvent> *writer) -> status;
  auto run_repair_pass(request_context *context,
                       const RunRepairPassRequest *request,
                       event_writer<RepairEvent> *writer) -> status;
  auto run_video_pass(request_context *context,
                      const RunVideoPassRequest *request,
                      event_writer<VideoEvent> *writer) -> status;
  auto commit(request_context *context, const CommitRequest *request,
              event_writer<CommitEvent> *writer) -> status;

private:
  auto require_session() -> std::optional<status>;
  auto try_begin_pass() -> std::optional<status>;
  /** Resolves a commit request's keep ids to sources; unknown ids go to
   * `errors`. Returns a status when the session cannot be read. */
  auto collect_commit_sources(const CommitRequest &request,
                              std::vector<commit_source> &sources,
                              std::vector<std::string> &errors)
      -> std::optional<status>;

  /// Best-effort: record the wizard's current step in the session DB so a
  /// later launch can offer to resume here. Logs and swallows DB errors —
  /// failing to checkpoint must never abort a pass.
  void record_step(int step) noexcept;

  /// Best-effort: mark the pass for `step` (a WizardStep index) as finished,
  /// so a resume restores its results instead of re-running it.
  void record_pass_complete(int step) noexcept;

  /// Best-effort: mark the pass for `step` finished for one batch only.
  void record_batch_pass_complete(int step,
                                  const std::string &batch_key) noexcept;

  /// Record a finished pass run: per-batch when `batch_key` is set,
  /// session-wide otherwise.
  void record_run_complete(int step, const std::string &batch_key) noexcept;

  bool has_active_session_ = false;
  std::filesystem::path session_folder_;
  database session_db_;
  std::atomic<bool> pass_active_{false};

  // Serializes EnsureModel (which is exempt from the single-pass lock and may
  // run alongside ingestion). `model_ready_` caches a verified on-disk model
  // for the process lifetime.
  std::mutex model_mutex_;
  bool model_ready_ = false;
};

} // namespace kustavi
