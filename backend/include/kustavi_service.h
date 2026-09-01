#pragma once

#include "collection/event_queue.h"
#include "pass/downscaler.h"
#include "pass/quality.h"
#include "store/database.h"

#include <grpcpp/grpcpp.h>
#include <proto/service.grpc.pb.h>

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

/// gRPC metadata key carrying the GUI-generated auth token.
inline constexpr std::string_view k_auth_token_header = "x-kustavi-auth-token";

/// Vision model reported by `GetInfo` (junk pass placeholder).
inline constexpr std::string_view k_model_name = "moondream2";

// --- per-pass streaming events --------------------------------------------
// Carried by event_queue between the pass worker and the gRPC writer thread.

struct scan_progress_evt {
  std::size_t files_seen = 0;
  std::size_t images_found = 0;
};
struct scan_image_evt {
  image::ingestion_result result;
};
struct scan_complete_evt {
  std::size_t images = 0;
  std::size_t skipped_files = 0;
  std::vector<std::string> errors;
};
using scan_event =
    std::variant<scan_progress_evt, scan_image_evt, scan_complete_evt>;

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
  uint32_t id = 0;
  std::int64_t start_unix_ms = 0;
  std::int64_t end_unix_ms = 0;
  std::vector<std::string> image_ids;
  std::optional<double> centroid_latitude;
  std::optional<double> centroid_longitude;
};
struct trips_complete_evt {
  std::size_t trips = 0;
  std::size_t unassigned = 0;
};
using trips_event =
    std::variant<trips_progress_evt, trips_result_evt, trips_complete_evt>;

struct commit_progress_evt {
  std::size_t done = 0;
  std::size_t total = 0;
  std::string current_name;
};
struct commit_complete_evt {
  std::size_t copied = 0;
  std::size_t skipped = 0;
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

/** Drains the pass event queue onto the gRPC stream.
 *
 * Checks for client cancellation on a 200 ms cadence; on cancellation or a
 * failed write it stops the producer and closes the queue.
 * @return OK when the stream completed, CANCELLED when the client went away.
 */
template <class ProtoEvent, class QueueEvent, class WriteFn>
auto stream_pass(grpc::ServerContext *ctx,
                 grpc::ServerWriter<ProtoEvent> *writer,
                 event_queue<QueueEvent> &queue,
                 std::stop_source &producer_stop, WriteFn write_one)
    -> grpc::Status {
  while (true) {
    const auto ev = queue.wait(std::chrono::milliseconds{200});
    if (ctx->IsCancelled()) {
      producer_stop.request_stop();
      queue.close();
      return grpc::Status(grpc::StatusCode::CANCELLED, "");
    }
    if (ev.has_value()) {
      if (!write_one(*ev)) {
        producer_stop.request_stop();
        queue.close();
        return grpc::Status(grpc::StatusCode::CANCELLED, "client disconnected");
      }
      continue;
    }
    if (queue.is_closed()) {
      return grpc::Status::OK;
    }
  }
}

/** Converts a producer failure (if any) into a gRPC INTERNAL status. */
inline auto producer_error_status(const std::exception_ptr &error)
    -> std::optional<grpc::Status> {
  if (error == nullptr) {
    return std::nullopt;
  }
  try {
    std::rethrow_exception(error);
  } catch (const std::exception &e) {
    return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
  } catch (...) {
    return grpc::Status(grpc::StatusCode::INTERNAL, "unknown internal error");
  }
}

// --- the service -------------------------------------------------------------

class kustavi_service : public ::kustavi::Kustavi::Service {
public:
  explicit kustavi_service(std::string auth_token);

  /** True once `Shutdown` was requested via gRPC. serve() polls this and
   * invokes `grpc::Server::Shutdown` from a dedicated thread — calling it
   * from a sync handler thread would deadlock (shutdown waits for the
   * in-flight handler). */
  auto is_shutdown_requested() const -> bool {
    return shutdown_requested_.load();
  }

  grpc::Status GetInfo(grpc::ServerContext *context,
                       const GetInfoRequest *request,
                       GetInfoResponse *response) override;
  grpc::Status Shutdown(grpc::ServerContext *context,
                        const ShutdownRequest *request,
                        ShutdownResponse *response) override;

  grpc::Status ScanFolder(grpc::ServerContext *context,
                          const ScanFolderRequest *request,
                          grpc::ServerWriter<ScanEvent> *writer) override;
  grpc::Status
  RunQualityPass(grpc::ServerContext *context,
                 const RunQualityPassRequest *request,
                 grpc::ServerWriter<QualityEvent> *writer) override;
  grpc::Status EnsureModel(grpc::ServerContext *context,
                           const EnsureModelRequest *request,
                           grpc::ServerWriter<ModelEvent> *writer) override;
  grpc::Status RunJunkPass(grpc::ServerContext *context,
                           const RunJunkPassRequest *request,
                           grpc::ServerWriter<JunkEvent> *writer) override;
  grpc::Status
  RunSimilarPass(grpc::ServerContext *context,
                 const RunSimilarPassRequest *request,
                 grpc::ServerWriter<SimilarEvent> *writer) override;
  grpc::Status RunTripsPass(grpc::ServerContext *context,
                            const RunTripsPassRequest *request,
                            grpc::ServerWriter<TripsEvent> *writer) override;
  grpc::Status Commit(grpc::ServerContext *context,
                      const CommitRequest *request,
                      grpc::ServerWriter<CommitEvent> *writer) override;

private:
  auto check_auth(const grpc::ServerContext *context) const -> bool;
  auto require_session() -> std::optional<grpc::Status>;
  auto try_begin_pass() -> std::optional<grpc::Status>;

  static auto unauthenticated() -> grpc::Status {
    return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                        "invalid or missing auth token");
  }

  std::string auth_token_;
  bool has_active_session_ = false;
  std::filesystem::path session_folder_;
  database session_db_;
  std::atomic<bool> pass_active_{false};
  std::atomic<bool> shutdown_requested_{false};

  // Serializes EnsureModel (which is exempt from the single-pass lock and may
  // run alongside ingestion). `model_ready_` caches a verified on-disk model
  // for the process lifetime.
  std::mutex model_mutex_;
  bool model_ready_ = false;
};

} // namespace kustavi
