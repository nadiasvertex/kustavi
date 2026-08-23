#include "pass/downscaler.h"
#include "exec/scheduler.h"
#include "exif.h"
#include "paths.h"
#include "store/database.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>
#include <stdexec/execution.hpp> // Include stdexec

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>

namespace ex = stdexec;
namespace fs = std::filesystem;

namespace kustavi::image {

namespace {

/** Deterministic 64-bit FNV-1a hash, stable across runs so cached working
 * images survive restarts. */
auto fnv1a64(std::string_view s) -> uint64_t {
  uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char c : s) {
    hash ^= static_cast<uint64_t>(c);
    hash *= 1099511628211ULL;
  }
  return hash;
}

auto hex64(uint64_t value) -> std::string {
  static constexpr std::string_view digits = "0123456789abcdef";
  std::string out(16, '0');
  for (std::size_t i = 16; i > 0; --i) {
    out[i - 1] = digits[value & 0xF];
    value >>= 4;
  }
  return out;
}

/** Forces '/' separators so image ids are platform-independent. */
auto slash_id(const fs::path &relative) -> std::string {
  std::string id = relative.generic_string();
  return id;
}
} // namespace

auto generate_working_image(const std::filesystem::path &base_path,
                            const std::filesystem::path &src_path,
                            const std::filesystem::path &cache_path)
    -> ingestion_result {
  ingestion_result result;
  const int target_max_dimension = 768;

  spdlog::debug("resizing '{}' -> '{}'", src_path.string(),
                cache_path.string());

  result.absolute_path = src_path;

  try {
    std::error_code size_ec;
    const auto file_size = fs::file_size(src_path, size_ec);
    if (size_ec) {
      result.error_message = "File size could not be read.";
      return result;
    }
    result.size_bytes = static_cast<std::int64_t>(file_size);

    // Key the cache entry on the relative id so identically named files in
    // different subfolders never collide.
    const auto relative_id = slash_id(fs::relative(src_path, base_path));
    std::string cached_filename = hex64(fnv1a64(relative_id)) + "_" +
                                  src_path.stem().string() + "_" +
                                  std::to_string(file_size) + ".jpg";
    fs::path dest_path = config::image_cache_path(cache_path) / cached_filename;

    result.relative_id = relative_id;
    result.working_path = dest_path.string();

    if (fs::exists(dest_path)) {
      cv::Mat header = cv::imread(src_path, cv::IMREAD_UNCHANGED);
      if (!header.empty()) {
        result.original_width = header.cols;
        result.original_height = header.rows;
        result.success = true;
        return result;
      }
    }

    cv::Mat src = cv::imread(src_path, cv::IMREAD_COLOR);
    if (src.empty()) {
      result.error_message = "File corrupt or encoding format unsupported.";
      return result;
    }

    result.original_width = src.cols;
    result.original_height = src.rows;

    int target_width = src.cols;
    int target_height = src.rows;

    if (src.cols > target_max_dimension || src.rows > target_max_dimension) {
      double scale = 0.0;
      if (src.cols >= src.rows) {
        scale = static_cast<double>(target_max_dimension) / src.cols;
      } else {
        scale = static_cast<double>(target_max_dimension) / src.rows;
      }
      target_width = static_cast<int>(src.cols * scale);
      target_height = static_cast<int>(src.rows * scale);
    }

    cv::Mat dst;
    cv::resize(src, dst, cv::Size(target_width, target_height), 0, 0,
               cv::INTER_LINEAR);

    std::vector<int> compression_params;
    compression_params.push_back(cv::IMWRITE_JPEG_QUALITY);
    compression_params.push_back(85);

    bool written = cv::imwrite(dest_path.string(), dst, compression_params);
    if (!written) {
      result.error_message =
          "Failed to write downscaled image matrix to cache.";
      return result;
    }

    result.success = true;
  } catch (const std::exception &e) {
    result.error_message =
        std::string("Internal downscaler exception: ") + e.what();
  }

  return result;
}

auto is_valid_image_file(const std::filesystem::path &path) -> bool {
  if (path.string().contains(".kustavi-cache")) {
    return false;
  }

  std::string ext = path.extension().string();
  if (ext.starts_with(".")) {
    ext.erase(0, 1);
  }
  std::ranges::transform(
      ext, ext.begin(), [](unsigned char c) -> int { return std::tolower(c); });

  return std::ranges::contains(supported_image_extensions, ext);
}

void insert_ingested_images(database &db,
                            const std::vector<ingestion_result> &images) {

  spdlog::debug("inserting {} images into database", images.size());

  db.begin_transaction();
  try {
    auto insert_stmt = db.prepare(R"(
            INSERT OR IGNORE INTO images (id, absolute_path, file_name, original_width, original_height, size_bytes, taken_unix_ms, latitude, longitude, working_image_path, scanned_at)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, strftime('%s','now'));
        )");

    for (const auto &img : images) {
      spdlog::debug("inserting image '{}' into database", img.relative_id);

      insert_stmt.bind_text(1, img.relative_id);
      insert_stmt.bind_path(2, img.absolute_path);
      insert_stmt.bind_path(3, img.absolute_path.filename());
      insert_stmt.bind_int(4, img.original_width);
      insert_stmt.bind_int(5, img.original_height);
      insert_stmt.bind_int64(6, img.size_bytes);
      if (img.taken_unix_ms.has_value()) {
        insert_stmt.bind_int64(7, *img.taken_unix_ms);
      } else {
        insert_stmt.bind_null(7);
      }
      if (img.latitude.has_value() && img.longitude.has_value()) {
        insert_stmt.bind_double(8, *img.latitude);
        insert_stmt.bind_double(9, *img.longitude);
      } else {
        insert_stmt.bind_null(8);
        insert_stmt.bind_null(9);
      }
      insert_stmt.bind_text(10, img.working_path);
      insert_stmt.step();
      insert_stmt.reset();
    }
    db.commit_transaction();
  } catch (...) {
    db.rollback_transaction();
    throw;
  }
}

auto execute_folder_ingestion_pass(
    database &db, const std::filesystem::path &source_folder, bool recursive,
    std::stop_token stop_token,
    const ingestion_progress_callback &progress_callback,
    const ingestion_image_callback &on_image) -> ingestion_summary {

  auto cache_dir = config::cache_path(source_folder);
  std::size_t files_seen = 0;
  std::size_t images_found = 0;
  std::vector<fs::path> paths_to_process;
  ingestion_summary summary;

  spdlog::debug("scanning '{}'", source_folder.string());

  const auto evaluate_entry = [&](const fs::directory_entry &entry) -> void {
    if (entry.is_directory()) {
      return;
    }
    files_seen++;

    const auto &abs_path = entry.path();
    spdlog::debug("evaluating '{}'", abs_path.string());

    if (is_valid_image_file(abs_path)) {
      images_found++;
      paths_to_process.push_back(abs_path);
    }
  };

  if (recursive) {
    for (const auto &entry : fs::recursive_directory_iterator(source_folder)) {
      if (stop_token.stop_requested()) {
        break;
      }
      evaluate_entry(entry);
      if (files_seen % 20 == 0) {
        progress_callback(files_seen, images_found, 0);
      }
    }
  } else {
    for (const auto &entry : fs::directory_iterator(source_folder)) {
      if (stop_token.stop_requested()) {
        break;
      }
      evaluate_entry(entry);
    }
  }

  progress_callback(files_seen, images_found, 0);

  summary.files_seen = files_seen;
  summary.images_found = images_found;

  // If there's nothing to do, return.
  if (paths_to_process.empty()) {
    return summary;
  }

  std::vector<ingestion_result> results(paths_to_process.size());
  std::atomic<std::size_t> successful_images{0};
  std::atomic<std::size_t> tasks_completed{0};
  std::mutex errors_mutex;

  auto scheduler = exec::make_scheduler();
  auto work =
      ex::schedule(scheduler) |
      ex::bulk(ex::par, paths_to_process.size(), [&](std::size_t idx) -> void {
        if (stop_token.stop_requested()) {
          return;
        }
        const auto &path = paths_to_process[idx];

        // Generate the image result and save directly to its distinct index
        auto result = generate_working_image(source_folder, path, cache_dir);

        if (result.success) {
          const auto metadata = exif::read_exif(path);
          result.taken_unix_ms = metadata.taken_unix_ms;
          result.latitude = metadata.latitude;
          result.longitude = metadata.longitude;

          successful_images.fetch_add(1, std::memory_order_relaxed);
          if (on_image) {
            on_image(result);
          }
        } else if (!result.error_message.empty()) {
          const std::string entry =
              result.relative_id + ": " + result.error_message;
          std::scoped_lock lock(errors_mutex);
          summary.errors.push_back(entry);
        }

        // Track overall workflow progress
        auto current_completed =
            tasks_completed.fetch_add(1, std::memory_order_relaxed) + 1;
        if (current_completed % 20 == 0) {
          progress_callback(files_seen, results.size(),
                            successful_images.load(std::memory_order_relaxed));
        }

        results[idx] = std::move(result);
      });

  // Wait for everything to finish before proceeding to the next step
  ex::sync_wait(work);

  summary.images_prepared = successful_images.load();

  // Save only the images we successfully prepared.
  std::vector<ingestion_result> prepared;
  prepared.reserve(summary.images_prepared);
  for (const auto &result : results) {
    if (result.success) {
      prepared.push_back(result);
    }
  }
  insert_ingested_images(db, prepared);

  return summary;
}

} // namespace kustavi::image
