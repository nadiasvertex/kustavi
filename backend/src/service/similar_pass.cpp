#include "kustavi_service.h"

#include "algorithm/append_range.h"
#include "pass/similar.h"
#include "paths.h"
#include "store/store.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kustavi {
namespace fs = std::filesystem;
using algorithm::append_range;

// ---------------------------------------------------------------------------
// Pass 4: similar images
// ---------------------------------------------------------------------------

auto kustavi_service::RunSimilarPass(grpc::ServerContext *context,
                                     const RunSimilarPassRequest *request,
                                     grpc::ServerWriter<SimilarEvent> *writer)
    -> grpc::Status {
  (void)request;
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

  std::vector<fs::path> paths;
  std::unordered_map<std::string, std::string> path_to_id;
  std::unordered_map<std::string, double> sharpness_by_id;
  try {
    const auto records = store::get_image_records(session_db_);
    paths.reserve(records.size());
    path_to_id.reserve(records.size());
    for (const auto &record : records) {
      paths.push_back(record.absolute_path);
      path_to_id.emplace(record.absolute_path.string(), record.id);
    }
    sharpness_by_id = store::get_quality_scores(session_db_);
  } catch (const std::exception &e) {
    return {grpc::StatusCode::INTERNAL,
            std::string("failed to read session: ") + e.what()};
  }

  // Composite "bestness" score: normalized sharpness when the quality pass
  // ran, otherwise neutral. s / (s + 100) maps Laplacian variance to 0..1
  // monotonically (100 -> 0.5, 1000 -> ~0.91).
  const auto score_of = [&](const fs::path &path) -> double {
    const auto id_it = path_to_id.find(path.string());
    const auto score_it = id_it != path_to_id.end()
                              ? sharpness_by_id.find(id_it->second)
                              : sharpness_by_id.end();
    if (score_it == sharpness_by_id.end()) {
      return 0.5;
    }
    return score_it->second / (score_it->second + 100.0);
  };

  event_queue<similar_event> queue;
  std::stop_source stop_source;
  std::exception_ptr producer_error;

  std::thread producer = run_producer(
      queue, stop_source, producer_error,
      [&](const std::stop_token &st) -> void {
        const auto groups = image::find_similar_images(
            image::default_similarity_radius, paths,
            [&](std::size_t done) -> void {
              queue.push(
                  similar_progress_evt{.done = done, .total = paths.size()});
            });

        std::vector<std::tuple<uint32_t, std::string, std::string, double>>
            rows;
        uint32_t group_id = 0;
        for (const auto &group : groups) {
          if (group.size() < 2) {
            continue;
          }
          struct member {
            const fs::path *path;
            double score;
          };
          std::vector<member> members;
          members.reserve(group.size());
          for (const auto &path : group) {
            members.push_back({.path = &path, .score = score_of(path)});
          }
          std::ranges::sort(members,
                            [](const member &a, const member &b) -> bool {
                              if (a.score != b.score) {
                                return a.score > b.score;
                              }
                              return a.path->string() < b.path->string();
                            });

          similar_group_evt evt;
          evt.image_ids.reserve(members.size());
          evt.member_scores.reserve(members.size());
          const auto keeper_path = members.front().path;
          const std::string keeper_id = path_to_id.at(keeper_path->string());
          evt.keeper_id = keeper_id;
          for (const auto &m : members) {
            const auto id = path_to_id.at(m.path->string());
            evt.image_ids.push_back(id);
            evt.member_scores.push_back(m.score);
            rows.emplace_back(group_id, id, keeper_id, m.score);
          }
          group_id++;
          queue.push(std::move(evt));
        }

        // Persist the group membership.
        session_db_.begin_transaction();
        try {
          auto stmt = session_db_.prepare("INSERT INTO similar_groups "
                                          "(group_id, image_id, keeper_id, "
                                          "score, processed_at) VALUES "
                                          "(?, ?, ?, ?, "
                                          "strftime('%s','now'));");
          for (const auto &[group, id, keeper, score] : rows) {
            stmt.bind_int(1, static_cast<int>(group));
            stmt.bind_text(2, id);
            stmt.bind_text(3, keeper);
            stmt.bind_double(4, score);
            stmt.step();
            stmt.reset();
          }
          session_db_.commit_transaction();
        } catch (...) {
          session_db_.rollback_transaction();
          throw;
        }
        queue.push(similar_complete_evt{.groups = group_id,
                                        .total_images = paths.size()});
      });

  uint32_t next_group_id = 0;
  grpc::Status status = stream_pass(
      context, writer, queue, stop_source,
      [&](const similar_event &ev) -> bool {
        SimilarEvent proto;
        std::visit(
            [&](const auto &e) -> auto {
              using evt = std::decay_t<decltype(e)>;
              if constexpr (std::is_same_v<evt, similar_progress_evt>) {
                auto *p = proto.mutable_progress();
                p->set_done(static_cast<uint32_t>(e.done));
                p->set_total(static_cast<uint32_t>(e.total));
              } else if constexpr (std::is_same_v<evt, similar_group_evt>) {
                auto *g = proto.mutable_group();
                g->set_id(next_group_id++);
                g->set_recommended_keep_id(e.keeper_id);
                append_range(g->mutable_image_ids(), e.image_ids);
                append_range(g->mutable_member_scores(), e.member_scores);
              } else {
                auto *c = proto.mutable_complete();
                c->set_groups(static_cast<uint32_t>(e.groups));
                c->set_total_images(static_cast<uint32_t>(e.total_images));
              }
            },
            ev);
        return writer->Write(proto);
      });

  producer.join();
  if (const auto err = producer_error_status(producer_error)) {
    return *err;
  }
  spdlog::info("similar pass finished");
  return status;
}
} // namespace kustavi
