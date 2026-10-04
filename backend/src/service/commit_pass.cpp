#include "kustavi_service.h"

#include "algorithm/append_range.h"
#include "pass/commit.h"
#include "paths.h"
#include "store/store.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kustavi {
namespace fs = std::filesystem;
using algorithm::append_range;

// ---------------------------------------------------------------------------
// Pass 6: commit
// ---------------------------------------------------------------------------

auto kustavi_service::collect_commit_sources(const CommitRequest &request,
                                             std::vector<commit_source> &sources,
                                             std::vector<std::string> &errors)
    -> std::optional<grpc::Status> {
  try {
    const auto records = store::get_image_records(session_db_);
    std::unordered_map<std::string, fs::path> id_to_path;
    id_to_path.reserve(records.size());
    for (const auto &record : records) {
      id_to_path.emplace(record.id, record.absolute_path);
    }
    const auto &folder_for_id = request.folder_for_id();
    sources.reserve(static_cast<std::size_t>(request.keep_ids_size()));
    for (const auto &id : request.keep_ids()) {
      const auto it = id_to_path.find(id);
      if (it == id_to_path.end()) {
        errors.push_back(id + ": not found in session");
        continue;
      }
      fs::path subdir;
      if (const auto fit = folder_for_id.find(id); fit != folder_for_id.end()) {
        const fs::path raw = fs::path(fit->second).lexically_normal();
        const bool escapes =
            raw.is_absolute() ||
            std::ranges::any_of(
                raw, [](const fs::path &part) -> bool { return part == ".."; });
        if (!escapes) {
          subdir = raw;
        }
      }
      sources.push_back(
          {.id = id, .path = it->second, .dest_subdir = std::move(subdir)});
    }
  } catch (const std::exception &e) {
    return grpc::Status{grpc::StatusCode::INTERNAL,
                        std::string("failed to read session: ") + e.what()};
  }
  return std::nullopt;
}

auto kustavi_service::EstimateCommit(grpc::ServerContext *context,
                                     const CommitRequest *request,
                                     EstimateCommitResponse *response)
    -> grpc::Status {
  if (!check_auth(context)) {
    return unauthenticated();
  }
  if (const auto err = require_session()) {
    return *err;
  }
  const fs::path destination{request->destination()};
  if (!destination.is_absolute()) {
    return {grpc::StatusCode::INVALID_ARGUMENT,
            "destination must be an absolute path"};
  }
  std::vector<commit_source> sources;
  std::vector<std::string> errors;
  if (const auto err = collect_commit_sources(*request, sources, errors)) {
    return *err;
  }
  const auto estimate = estimate_commit(
      session_folder_, destination, sources,
      {.merge_existing = request->merge_existing()});
  response->set_total_bytes(estimate.total_bytes);
  response->set_new_bytes(estimate.new_bytes);
  response->set_already_present(
      static_cast<uint32_t>(estimate.already_present));
  response->set_free_bytes_known(estimate.free_bytes.has_value());
  response->set_free_bytes(estimate.free_bytes.value_or(0));
  response->set_fits(!estimate.free_bytes ||
                     estimate.new_bytes <= *estimate.free_bytes);
  return grpc::Status::OK;
}

auto kustavi_service::Commit(grpc::ServerContext *context,
                             const CommitRequest *request,
                             grpc::ServerWriter<CommitEvent> *writer)
    -> grpc::Status {
  if (!check_auth(context)) {
    return unauthenticated();
  }
  if (const auto err = require_session()) {
    return *err;
  }
  if (const auto err = try_begin_pass()) {
    return *err;
  }
  pass_guard guard(pass_active_, true);
  record_step(6); // WizardStep.copy

  const auto &destination_str = request->destination();
  if (destination_str.empty()) {
    return {grpc::StatusCode::INVALID_ARGUMENT, "destination is required"};
  }
  const fs::path destination{destination_str};
  if (!destination.is_absolute()) {
    return {grpc::StatusCode::INVALID_ARGUMENT,
            "destination must be an absolute path"};
  }

  std::vector<commit_source> sources;
  std::vector<std::string> errors;
  if (const auto err = collect_commit_sources(*request, sources, errors)) {
    return *err;
  }
  const commit_options options{.merge_existing = request->merge_existing()};

  const auto estimate =
      estimate_commit(session_folder_, destination, sources, options);
  if (estimate.free_bytes && estimate.new_bytes > *estimate.free_bytes) {
    return {grpc::StatusCode::FAILED_PRECONDITION,
            "not enough free space at the destination: " +
                std::to_string(estimate.new_bytes) + " bytes needed, " +
                std::to_string(*estimate.free_bytes) + " available"};
  }

  event_queue<commit_event> queue;
  std::stop_source stop_source;
  std::exception_ptr producer_error;

  std::thread producer = run_producer(
      queue, stop_source, producer_error,
      [&](const std::stop_token &st) -> void {
        auto summary =
            commit_files(session_folder_, destination, sources, st,
                         [&](std::size_t done, std::size_t total,
                             const fs::path &current) -> void {
                           queue.push(commit_progress_evt{
                               .done = done,
                               .total = total,
                               .current_name = current.filename().string()});
                         },
                         options);
        for (const auto &error : errors) {
          summary.errors.push_back(error);
        }
        queue.push(commit_complete_evt{.copied = summary.copied,
                                       .skipped = summary.skipped,
                                       .already_present = summary.already_present,
                                       .companions = summary.companions,
                                       .errors = std::move(summary.errors)});
      });

  grpc::Status status = stream_pass(
      context, writer, queue, stop_source, [&](const commit_event &ev) -> bool {
        CommitEvent proto;
        std::visit(
            [&](const auto &e) -> auto {
              using evt = std::decay_t<decltype(e)>;
              if constexpr (std::is_same_v<evt, commit_progress_evt>) {
                auto *p = proto.mutable_progress();
                p->set_done(static_cast<uint32_t>(e.done));
                p->set_total(static_cast<uint32_t>(e.total));
                p->set_current_name(e.current_name);
              } else {
                auto *c = proto.mutable_complete();
                c->set_copied(static_cast<uint32_t>(e.copied));
                c->set_skipped(static_cast<uint32_t>(e.skipped));
                c->set_companions(static_cast<uint32_t>(e.companions));
                c->set_already_present(
                    static_cast<uint32_t>(e.already_present));
                append_range(c->mutable_errors(), e.errors);
              }
            },
            ev);
        return writer->Write(proto);
      });

  producer.join();
  if (const auto err = producer_error_status(producer_error)) {
    return *err;
  }
  spdlog::info("commit finished");
  return status;
}

} // namespace kustavi
