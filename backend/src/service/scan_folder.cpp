#include "kustavi_service.h"

#include "algorithm/append_range.h"
#include "paths.h"
#include "store/store.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <utility>
#include <variant>

namespace kustavi {

namespace fs = std::filesystem;
using algorithm::append_range;

// ---------------------------------------------------------------------------
// Pass 1: scan & thumbnails
// ---------------------------------------------------------------------------

auto kustavi_service::ScanFolder(grpc::ServerContext *context,
                                 const ScanFolderRequest *request,
                                 grpc::ServerWriter<ScanEvent> *writer)
    -> grpc::Status {
  if (!check_auth(context)) {
    return unauthenticated();
  }
  if (const auto err = try_begin_pass()) {
    return *err;
  }
  pass_guard guard(pass_active_, true);

  const auto &folder_str = request->folder();
  if (folder_str.empty()) {
    return {grpc::StatusCode::INVALID_ARGUMENT, "folder is required"};
  }
  const fs::path folder{folder_str};
  std::error_code ec;
  if (!fs::is_directory(folder, ec) || ec) {
    return {grpc::StatusCode::NOT_FOUND,
            "folder does not exist: " + folder_str};
  }

  // A new scan starts a fresh session: discard the previous index and cache.
  try {
    session_db_.open(folder);
    store::reset_session(session_db_);
  } catch (const std::exception &e) {
    return {grpc::StatusCode::INTERNAL,
            std::string("failed to start session: ") + e.what()};
  }

  const auto image_cache = config::image_cache_path(config::cache_path(folder));
  if (fs::exists(image_cache)) {
    std::error_code wipe_ec;
    fs::remove_all(image_cache, wipe_ec);
    fs::create_directories(image_cache, wipe_ec);
  }

  has_active_session_ = true;
  session_folder_ = folder;
  spdlog::info("scan started for '{}'", folder_str);

  event_queue<scan_event> queue;
  std::stop_source stop_source;
  std::exception_ptr producer_error;
  std::size_t next_progress_at = 250;

  std::thread producer = run_producer(
      queue, stop_source, producer_error, [&](std::stop_token st) -> void {
        const auto summary = image::execute_folder_ingestion_pass(
            session_db_, folder, request->recursive(), std::move(st),
            [&](std::size_t files_seen, std::size_t images_found,
                std::size_t) -> void {
              if (files_seen >= next_progress_at) {
                next_progress_at = files_seen + 250;
                queue.push(scan_progress_evt{.files_seen = files_seen,
                                             .images_found = images_found});
              }
            },
            [&](const image::ingestion_result &result) -> void {
              queue.push(scan_image_evt{result});
            });
        queue.push(scan_complete_evt{.images = summary.images_prepared,
                                     .skipped_files = summary.files_seen -
                                                      summary.images_found,
                                     .errors = summary.errors});
      });

  grpc::Status status = stream_pass(
      context, writer, queue, stop_source, [&](const scan_event &ev) -> bool {
        ScanEvent proto;
        std::visit(
            [&](const auto &e) -> auto {
              using evt = std::decay_t<decltype(e)>;
              if constexpr (std::is_same_v<evt, scan_progress_evt>) {
                auto *p = proto.mutable_progress();
                p->set_files_seen(static_cast<uint32_t>(e.files_seen));
                p->set_images_found(static_cast<uint32_t>(e.images_found));
              } else if constexpr (std::is_same_v<evt, scan_image_evt>) {
                auto *m = proto.mutable_image();
                const auto &r = e.result;
                m->set_id(r.relative_id);
                m->set_path(r.absolute_path.string());
                m->set_name(r.absolute_path.filename().string());
                m->set_width(static_cast<uint32_t>(r.original_width));
                m->set_height(static_cast<uint32_t>(r.original_height));
                m->set_size_bytes(static_cast<uint64_t>(r.size_bytes));
                if (r.taken_unix_ms.has_value()) {
                  m->set_taken_unix_ms(*r.taken_unix_ms);
                }
                if (r.latitude.has_value() && r.longitude.has_value()) {
                  auto *gps = m->mutable_gps();
                  gps->set_latitude(*r.latitude);
                  gps->set_longitude(*r.longitude);
                }
                m->set_thumbnail_path(r.working_path);
                m->set_kind(r.kind == image::media_kind_video
                                ? MediaKind::VIDEO
                                : MediaKind::PHOTO);
                if (r.duration_ms.has_value()) {
                  m->set_duration_ms(*r.duration_ms);
                }
              } else {
                auto *c = proto.mutable_complete();
                c->set_images(static_cast<uint32_t>(e.images));
                c->set_skipped_files(static_cast<uint32_t>(e.skipped_files));
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
  spdlog::info("scan finished");
  return status;
}
} // namespace kustavi
