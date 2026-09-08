#include "kustavi_service.h"

#include "paths.h"
#include "store/store.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace kustavi {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Session resume: probe a folder for saved progress (InspectSession), rehydrate
// the slow passes' results and the user's decisions (GetSessionResults), and
// persist the wizard's position as the user works (SaveSessionState).
//
// `wizard_step` is a WizardStep index, kept in sync with the Dart enum in
// frontend/lib/src/state/phases.dart:
//   0 select | 1 quality | 2 duplicates | 3 junk | 4 video | 5 trips | 6 copy
// A pass stamps its own index when it starts, so an interrupted pass resumes
// into itself; the front end also stamps on review-screen navigation.
// ---------------------------------------------------------------------------

namespace {

// session_state keys for the wizard tunables.
constexpr std::string_view k_blur_key = "blur_threshold";
constexpr std::string_view k_under_key = "underexposed_threshold";
constexpr std::string_view k_over_key = "overexposed_threshold";
constexpr std::string_view k_gap_key = "trip_gap_hours";
constexpr std::string_view k_distance_key = "trip_distance_km";
constexpr std::string_view k_home_key = "trip_home_radius_km";
constexpr std::string_view k_leg_key = "trip_leg_radius_km";
constexpr std::string_view k_keepers_key = "group_keepers";

auto parse_double(const std::optional<std::string> &raw) -> double {
  if (!raw) {
    return 0.0;
  }
  try {
    return std::stod(*raw);
  } catch (const std::exception &) {
    return 0.0;
  }
}

auto parse_int(const std::optional<std::string> &raw) -> int {
  if (!raw) {
    return 0;
  }
  try {
    return std::stoi(*raw);
  } catch (const std::exception &) {
    return 0;
  }
}

// Similar-group keeper overrides serialize as "<group_id>\t<keeper_id>\n"
// lines. Image ids are folder-relative paths, which never contain a tab or
// newline.
template <class KeeperMap>
auto encode_group_keepers(const KeeperMap &keepers) -> std::string {
  std::string out;
  for (const auto &[group_id, keeper_id] : keepers) {
    out += std::to_string(group_id);
    out += '\t';
    out += keeper_id;
    out += '\n';
  }
  return out;
}

template <class KeeperMap>
void decode_group_keepers(const std::string &encoded, KeeperMap *out) {
  std::string_view rest{encoded};
  while (!rest.empty()) {
    const auto nl = rest.find('\n');
    const std::string_view line =
        nl == std::string_view::npos ? rest : rest.substr(0, nl);
    rest =
        nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
    const auto tab = line.find('\t');
    if (tab == std::string_view::npos) {
      continue;
    }
    unsigned int group_id = 0;
    const auto id_text = line.substr(0, tab);
    const auto [ptr, ec] = std::from_chars(
        id_text.data(), id_text.data() + id_text.size(), group_id);
    if (ec != std::errc{}) {
      continue;
    }
    (*out)[group_id] = std::string(line.substr(tab + 1));
  }
}

auto pass_complete_key(int step) -> std::string {
  return "pass_" + std::to_string(step) + "_complete";
}

} // namespace

void kustavi_service::record_step(int step) noexcept {
  try {
    store::set_wizard_step(session_db_, step);
  } catch (const std::exception &e) {
    spdlog::warn("could not record wizard step {}: {}", step, e.what());
  }
}

void kustavi_service::record_pass_complete(int step) noexcept {
  try {
    store::set_session_value(session_db_, pass_complete_key(step), "1");
  } catch (const std::exception &e) {
    spdlog::warn("could not record pass {} completion: {}", step, e.what());
  }
}

auto kustavi_service::InspectSession(grpc::ServerContext *context,
                                     const InspectSessionRequest *request,
                                     InspectSessionResponse *response)
    -> grpc::Status {
  if (!check_auth(context)) {
    return unauthenticated();
  }

  const auto &folder_str = request->folder();
  if (folder_str.empty()) {
    return {grpc::StatusCode::INVALID_ARGUMENT, "folder is required"};
  }
  const fs::path folder{folder_str};

  // A missing DB file means no session — don't open (which would create the
  // cache dir and an empty DB as a side effect).
  const auto db_path = config::session_db_path(config::cache_path(folder));
  std::error_code ec;
  if (!fs::exists(db_path, ec) || ec) {
    return grpc::Status::OK; // has_session stays false
  }

  try {
    database probe;
    probe.open(folder);
    if (!store::session_has_index(probe)) {
      return grpc::Status::OK;
    }
    response->set_has_session(true);

    auto count = probe.prepare("SELECT COUNT(*) FROM images;");
    if (count.step() == SQLITE_ROW) {
      response->set_image_count(
          static_cast<std::uint32_t>(sqlite3_column_int64(count.raw(), 0)));
    }
    response->set_resume_step(
        static_cast<std::uint32_t>(store::get_wizard_step(probe)));
    response->set_updated_unix_ms(
        parse_int(store::get_session_value(probe, "wizard_step_updated_at")) *
        1000LL);
  } catch (const std::exception &e) {
    return {grpc::StatusCode::INTERNAL,
            std::string("failed to inspect session: ") + e.what()};
  }
  return grpc::Status::OK;
}

auto kustavi_service::GetSessionResults(grpc::ServerContext *context,
                                        const GetSessionResultsRequest *request,
                                        GetSessionResultsResponse *response)
    -> grpc::Status {
  (void)request;
  if (!check_auth(context)) {
    return unauthenticated();
  }
  if (const auto err = require_session()) {
    return *err;
  }

  try {
    {
      // Flagged photos only (reasons bitmask: BLURRY=1, UNDER=2, OVER=4).
      auto stmt = session_db_.prepare(
          "SELECT image_id, focus_peak, underexposed, overexposed, reasons "
          "FROM quality_flags WHERE reasons != 0;");
      while (stmt.step() == SQLITE_ROW) {
        auto *flag = response->add_quality_flags();
        const auto *id =
            reinterpret_cast<const char *>(sqlite3_column_text(stmt.raw(), 0));
        if (id != nullptr) {
          flag->set_image_id(id);
        }
        flag->set_sharpness(sqlite3_column_double(stmt.raw(), 1));
        const double under = sqlite3_column_double(stmt.raw(), 2);
        const double over = sqlite3_column_double(stmt.raw(), 3);
        flag->set_exposure_score(
            std::clamp(0.5 - std::max(under, over), 0.0, 0.5));
        const int mask = sqlite3_column_int(stmt.raw(), 4);
        if ((mask & 1) != 0) {
          flag->add_reasons(BLURRY);
        }
        if ((mask & 2) != 0) {
          flag->add_reasons(UNDER_EXPOSED);
        }
        if ((mask & 4) != 0) {
          flag->add_reasons(OVER_EXPOSED);
        }
      }
    }
    {
      // Regroup the flat similar_groups table by group_id.
      auto stmt = session_db_.prepare(
          "SELECT group_id, image_id, keeper_id, score FROM similar_groups "
          "ORDER BY group_id;");
      SimilarGroup *group = nullptr;
      int current_group = -1;
      while (stmt.step() == SQLITE_ROW) {
        const int gid = sqlite3_column_int(stmt.raw(), 0);
        const auto *image_id =
            reinterpret_cast<const char *>(sqlite3_column_text(stmt.raw(), 1));
        const auto *keeper_id =
            reinterpret_cast<const char *>(sqlite3_column_text(stmt.raw(), 2));
        if (group == nullptr || gid != current_group) {
          group = response->add_similar_groups();
          group->set_id(static_cast<std::uint32_t>(gid));
          if (keeper_id != nullptr) {
            group->set_recommended_keep_id(keeper_id);
          }
          current_group = gid;
        }
        if (image_id != nullptr) {
          group->add_image_ids(image_id);
        }
        group->add_member_scores(sqlite3_column_double(stmt.raw(), 3));
      }
    }
    {
      auto stmt = session_db_.prepare("SELECT image_id, reason, confidence "
                                      "FROM junk_flags WHERE is_junk = 1;");
      while (stmt.step() == SQLITE_ROW) {
        auto *flag = response->add_junk_flags();
        const auto *id =
            reinterpret_cast<const char *>(sqlite3_column_text(stmt.raw(), 0));
        const auto *reason =
            reinterpret_cast<const char *>(sqlite3_column_text(stmt.raw(), 1));
        if (id != nullptr) {
          flag->set_image_id(id);
        }
        if (reason != nullptr) {
          flag->set_reason(reason);
        }
        flag->set_confidence(sqlite3_column_double(stmt.raw(), 2));
      }
    }
    {
      auto stmt = session_db_.prepare("SELECT video_id, reason, confidence "
                                      "FROM video_flags WHERE is_junk = 1;");
      while (stmt.step() == SQLITE_ROW) {
        auto *flag = response->add_video_flags();
        const auto *id =
            reinterpret_cast<const char *>(sqlite3_column_text(stmt.raw(), 0));
        const auto *reason =
            reinterpret_cast<const char *>(sqlite3_column_text(stmt.raw(), 1));
        if (id != nullptr) {
          flag->set_video_id(id);
        }
        if (reason != nullptr) {
          flag->set_reason(reason);
        }
        flag->set_confidence(sqlite3_column_double(stmt.raw(), 2));
      }
    }
    {
      auto stmt = session_db_.prepare(
          "SELECT COUNT(*) FROM images WHERE kind = 'video';");
      if (stmt.step() == SQLITE_ROW) {
        response->set_video_total(
            static_cast<std::uint32_t>(sqlite3_column_int64(stmt.raw(), 0)));
      }
    }

    for (const auto &row : store::get_user_decisions(session_db_)) {
      auto *entry = response->add_decisions();
      entry->set_image_id(row.image_id);
      entry->set_decision(row.remove ? Decision::DELETE : Decision::KEEP);
    }

    response->set_resume_step(
        static_cast<std::uint32_t>(store::get_wizard_step(session_db_)));

    const auto done = [&](int step) -> bool {
      return store::get_session_value(session_db_, pass_complete_key(step))
          .has_value();
    };
    response->set_quality_done(done(1));
    response->set_similar_done(done(2));
    response->set_junk_done(done(3));
    response->set_video_done(done(4));

    auto *thresholds = response->mutable_quality_thresholds();
    thresholds->set_blur_threshold(
        parse_double(store::get_session_value(session_db_, k_blur_key)));
    thresholds->set_underexposed_threshold(
        parse_double(store::get_session_value(session_db_, k_under_key)));
    thresholds->set_overexposed_threshold(
        parse_double(store::get_session_value(session_db_, k_over_key)));

    auto *trips = response->mutable_trip_params();
    trips->set_max_gap_hours(
        parse_int(store::get_session_value(session_db_, k_gap_key)));
    trips->set_max_distance_km(
        parse_int(store::get_session_value(session_db_, k_distance_key)));
    trips->set_home_radius_km(
        parse_int(store::get_session_value(session_db_, k_home_key)));
    trips->set_leg_radius_km(
        parse_int(store::get_session_value(session_db_, k_leg_key)));

    if (const auto encoded =
            store::get_session_value(session_db_, k_keepers_key)) {
      decode_group_keepers(*encoded, response->mutable_group_keepers());
    }
  } catch (const std::exception &e) {
    return {grpc::StatusCode::INTERNAL,
            std::string("failed to read session results: ") + e.what()};
  }
  return grpc::Status::OK;
}

auto kustavi_service::SaveSessionState(grpc::ServerContext *context,
                                       const SaveSessionStateRequest *request,
                                       SaveSessionStateResponse *response)
    -> grpc::Status {
  (void)response;
  if (!check_auth(context)) {
    return unauthenticated();
  }
  if (const auto err = require_session()) {
    return *err;
  }

  try {
    if (request->has_step()) {
      store::set_wizard_step(session_db_, static_cast<int>(request->step()));
    }
    if (request->has_quality_thresholds()) {
      const auto &t = request->quality_thresholds();
      store::set_session_value(session_db_, k_blur_key,
                               std::to_string(t.blur_threshold()));
      store::set_session_value(session_db_, k_under_key,
                               std::to_string(t.underexposed_threshold()));
      store::set_session_value(session_db_, k_over_key,
                               std::to_string(t.overexposed_threshold()));
    }
    if (request->has_trip_params()) {
      const auto &p = request->trip_params();
      store::set_session_value(session_db_, k_gap_key,
                               std::to_string(p.max_gap_hours()));
      store::set_session_value(session_db_, k_distance_key,
                               std::to_string(p.max_distance_km()));
      store::set_session_value(session_db_, k_home_key,
                               std::to_string(p.home_radius_km()));
      store::set_session_value(session_db_, k_leg_key,
                               std::to_string(p.leg_radius_km()));
    }
    if (request->has_replace_decisions() && request->replace_decisions()) {
      std::vector<store::user_decision_row> rows;
      rows.reserve(static_cast<std::size_t>(request->decisions_size()));
      for (const auto &entry : request->decisions()) {
        if (entry.decision() == Decision::DECISION_UNSPECIFIED) {
          continue;
        }
        rows.push_back({.image_id = entry.image_id(),
                        .remove = entry.decision() == Decision::DELETE});
      }
      store::replace_user_decisions(session_db_, rows);
    }
    if (!request->group_keepers().empty()) {
      store::set_session_value(session_db_, k_keepers_key,
                               encode_group_keepers(request->group_keepers()));
    }
  } catch (const std::exception &e) {
    return {grpc::StatusCode::INTERNAL,
            std::string("failed to save session state: ") + e.what()};
  }
  return grpc::Status::OK;
}

} // namespace kustavi
