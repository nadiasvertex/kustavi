#include "downscaler.h"
#include "database.h"
#include "paths.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_set>

namespace fs = std::filesystem;

namespace kustavi::image {

auto generate_working_image(const std::filesystem::path &base_path,
                            const std::filesystem::path &src_path,
                            const std::filesystem::path &cache_path)
    -> ingestion_result {
  ingestion_result result;
  const int target_max_dimension = 768;

  spdlog::debug("resizing '{}' -> '{}'", src_path.string(),
                cache_path.string());

  try {
    std::string cached_filename = src_path.stem().string() + "_" +
                                  std::to_string(fs::file_size(src_path)) +
                                  ".jpg";
    fs::path dest_path = config::image_cache_path(cache_path) / cached_filename;

    result.relative_id = fs::relative(src_path, base_path).string();
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

bool is_valid_image_file(const std::filesystem::path &path) {
  if (path.string().contains(".kustavi-cache")) {
    return false;
  }

  static const std::unordered_set<std::string> valid_extensions = {
      ".jpg", ".jpeg", ".png", ".webp"};

  auto ext = path.extension().string();
  std::ranges::transform(ext, ext.begin(), ::tolower);

  return valid_extensions.contains(ext);
}

void insert_ingested_images(database &db,
                            const std::vector<ingestion_result> &images) {
  db.begin_transaction();
  try {
    auto insert_stmt = db.prepare(R"(
            INSERT OR IGNORE INTO images (id, absolute_path, file_name, original_width, original_height, size_bytes, working_image_path, scanned_at)
            VALUES (?, ?, ?, ?, ?, ?, ?, strftime('%s','now'));
        )");

    for (const auto &img : images) {
      insert_stmt.bind_text(1, img.relative_id);
      insert_stmt.bind_text(2, img.absolute_path.string());
      insert_stmt.bind_path(3, img.absolute_path.filename());
      insert_stmt.bind_int(4, img.original_width);
      insert_stmt.bind_int(5, img.original_height);
      insert_stmt.bind_int64(6, img.size_bytes);
      insert_stmt.bind_text(7, img.working_path);
      insert_stmt.step();
    }
    db.commit_transaction();
  } catch (...) {
    db.rollback_transaction();
    throw;
  }
}

void execute_folder_ingestion_pass(
    database &db, const std::filesystem::path &source_folder,
    const std::function<void(std::size_t files_seen, std::size_t images_found)>
        &progress_callback) {

  auto cache_dir = config::cache_path(source_folder);
  std::size_t files_seen = 0;
  std::vector<ingestion_result> results;

  spdlog::debug("scanning '{}'", source_folder.string());
  for (const auto &entry : fs::recursive_directory_iterator(source_folder)) {
    files_seen++;

    const auto &abs_path = entry.path();
    spdlog::debug("evaluating '{}'", abs_path.string());

    if (!is_valid_image_file(abs_path)) {
      continue;
    }

    results.emplace_back(
        generate_working_image(source_folder, abs_path, cache_dir));

    if (files_seen % 20 == 0) {
      progress_callback(files_seen, results.size());
    }
  }

  progress_callback(files_seen, results.size());

  // Save all the of the images we located.
  insert_ingested_images(db, results);
}

} // namespace kustavi::image
