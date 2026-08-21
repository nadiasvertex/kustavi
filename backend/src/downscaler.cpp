#include "downscaler.h"
#include "database.h"
#include "paths.h"

#include <algorithm>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <filesystem>
#include <print>
#include <string>
#include <unordered_set>

namespace fs = std::filesystem;

namespace kustavi::image {

auto generate_working_image(const std::filesystem::path &src_path,
                            const std::filesystem::path &cache_path)
    -> ingestion_result {
  ingestion_result result;
  const int target_max_dimension = 768;

  try {
    std::string cached_filename = src_path.stem().string() + "_" +
                                  std::to_string(fs::file_size(src_path)) +
                                  ".jpg";
    fs::path dest_path = config::image_cache_path(cache_path) / cached_filename;

    result.working_image_path = dest_path.string();

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

void execute_folder_ingestion_pass(
    database &db, const std::string &source_folder,
    const std::function<void(int files_seen, int images_found)>
        &progress_callback) {

  std::string cache_dir = (fs::path(source_folder) / ".kustavi-cache").string();
  int files_seen = 0;
  int images_found = 0;

  const std::unordered_set<std::string> valid_extensions = {".jpg", ".jpeg",
                                                            ".png", ".webp"};

  db.begin_transaction();
  try {
    auto insert_stmt = db.prepare(R"(
            INSERT OR IGNORE INTO images (id, absolute_path, file_name, original_width, original_height, size_bytes, working_image_path, scanned_at)
            VALUES (?, ?, ?, ?, ?, ?, ?, strftime('%s','now'));
        )");

    for (const auto &entry : fs::recursive_directory_iterator(source_folder)) {
      files_seen++;

      if (entry.path().string().find(".kustavi-cache") != std::string::npos) {
        continue;
      }

      std::string ext = entry.path().extension().string();
      std::ranges::transform(ext, ext.begin(), ::tolower);

      if (valid_extensions.count(ext) == 0) {
        continue;
      }

      images_found++;
      std::string abs_path = entry.path().string();
      std::string relative_id =
          fs::relative(entry.path(), source_folder).string();

      ingestion_result resize_result =
          generate_working_image(abs_path, cache_dir);

      if (resize_result.success) {
        insert_stmt.bind_text(1, relative_id);
        insert_stmt.bind_text(2, abs_path);
        insert_stmt.bind_text(3, entry.path().filename().string());
        insert_stmt.bind_int(4, resize_result.original_width);
        insert_stmt.bind_int(5, resize_result.original_height);
        insert_stmt.bind_int64(6, fs::file_size(entry.path()));
        insert_stmt.bind_text(7, resize_result.working_image_path);

        insert_stmt.step();
      } else {
        std::print("Skipping file '{}' because: '{}'\n", relative_id,
                   resize_result.error_message);
      }

      if (files_seen % 20 == 0) {
        progress_callback(files_seen, images_found);
      }
    }

    db.commit_transaction();
  } catch (...) {
    db.rollback_transaction();
    throw;
  }

  progress_callback(files_seen, images_found);
}

} // namespace kustavi::image
