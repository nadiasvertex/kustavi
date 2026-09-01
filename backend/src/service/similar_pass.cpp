#include "kustavi_service.h"

#include "algorithm/append_range.h"
#include "pass/keeper_signals.h"
#include "pass/similar.h"
#include "paths.h"
#include "store/store.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <unordered_set>
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

  // Images the user marked for deletion in the quality step: never scored,
  // never eligible as a group keeper. A group left with < 2 survivors is not
  // emitted (the lone survivor is simply kept).
  std::unordered_set<std::string> skip(request->skip_image_ids().begin(),
                                       request->skip_image_ids().end());

  // Sharpness term: normalized Laplacian variance from the quality pass, or a
  // neutral 0.5 when that pass did not score this image. s / (s + 100) maps
  // variance to 0..1 monotonically (100 -> 0.5, 1000 -> ~0.91).
  const auto sharpness_term = [&](const fs::path &path) -> double {
    const auto id_it = path_to_id.find(path.string());
    const auto score_it = id_it != path_to_id.end()
                              ? sharpness_by_id.find(id_it->second)
                              : sharpness_by_id.end();
    if (score_it == sharpness_by_id.end()) {
      return 0.5;
    }
    return score_it->second / (score_it->second + 100.0);
  };

  // Optional face/eye + color-balance analysis. Missing model degrades to
  // sharpness + color-balance only (color balance is still computed per image
  // via the free helper below).
  std::optional<image::keeper_analyzer> analyzer;
  if (auto loaded = image::keeper_analyzer::load(config::face_model_path())) {
    analyzer.emplace(std::move(*loaded));
  } else {
    spdlog::warn("similar: keeper-signal analysis unavailable ({}); keeper "
                 "falls back to sharpness + color balance",
                 loaded.error());
  }

  // Blend the sharpness term with the keeper signals. Weights are tunable;
  // neutral metrics leave the sharpness term unchanged.
  constexpr double k_w_face_focus = 0.15; // faces present AND largest in focus
  constexpr double k_w_group_shot = 0.10; // more faces visible (group shot)
  constexpr double k_w_color = 0.10;      // white-balance neutrality
  constexpr double k_w_closed = 0.25;     // closed / blinking eyes
  constexpr double k_w_redeye = 0.20;     // visible red-eye
  const auto blended_score = [&](const fs::path &path) -> double {
    const double s_term = sharpness_term(path);

    image::keeper_metrics km;
    if (analyzer) {
      km = analyzer->analyze(path);
    }
    if (!km.valid) {
      km = image::keeper_metrics{};
      km.color_balance = image::color_balance_score(path);
    }

    const double face_focus_bonus =
        km.face_count > 0 ? km.largest_face_focus : 0.0;
    const double group_shot = std::min(km.face_count, 3) / 3.0;
    const double score = s_term + (k_w_face_focus * face_focus_bonus) +
                         (k_w_group_shot * group_shot) +
                         (k_w_color * km.color_balance) -
                         (k_w_closed * (1.0 - km.eyes_open_ratio)) -
                         (k_w_redeye * km.redeye_ratio);
    return std::clamp(score, 0.0, 1.0);
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
            const auto id_it = path_to_id.find(path.string());
            if (id_it != path_to_id.end() && skip.contains(id_it->second)) {
              continue; // marked for deletion upstream
            }
            members.push_back({.path = &path, .score = blended_score(path)});
          }
          // The exclusion above can collapse a group below the 2-member floor.
          if (members.size() < 2) {
            continue;
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
