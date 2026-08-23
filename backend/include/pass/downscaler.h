#pragma once

#include "store/database.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace kustavi::image {

/// Lowercase file extensions of the image formats the back end can decode.
inline constexpr std::array<std::string_view, 4> supported_image_extensions{
    "jpg", "jpeg", "png", "webp"};

/**
 * @brief Represents the result of an image ingestion operation.
 */
struct ingestion_result {
  bool success = false; //! Indicates whether the ingestion was successful.
  std::string relative_id;
  std::filesystem::path absolute_path;
  std::int32_t original_width = 0;  //! The original width of the source image.
  std::int32_t original_height = 0; //! The original height of the source image.
  std::int64_t size_bytes = 0;
  std::string working_path; //! The path to the generated working image in the
                            //! cache.
  std::optional<std::int64_t> taken_unix_ms; //! EXIF capture time, if present.
  std::optional<double> latitude;            //! EXIF GPS, if present.
  std::optional<double> longitude;           //! EXIF GPS, if present.
  std::string error_message; //! An error message describing the reason for
                             //! failure, if any.
};

/**
 * @brief Summarizes a folder ingestion pass.
 */
struct ingestion_summary {
  std::size_t files_seen = 0;      //! Regular files encountered (any type).
  std::size_t images_found = 0;    //! Files with a supported image extension.
  std::size_t images_prepared = 0; //! Images successfully prepared.
  std::vector<std::string> errors; //! "<id>: <reason>" per unreadable image.
};

using ingestion_progress_callback =
    std::function<void(std::size_t files_seen, std::size_t images_found,
                       std::size_t images_prepared)>;

/** Called (from scheduler threads) once an image's working file is on disk. */
using ingestion_image_callback = std::function<void(const ingestion_result &)>;

/**
 * @brief Generates a working image from the source image and stores it in the
 * cache directory.
 *
 * @param base_path The session folder the source image belongs to.
 * @param src_path The path to the source image.
 * @param cache_path The path to the cache directory where the working image
 * will be stored.
 * @return ingestion_result The result of the ingestion operation, including
 * success status, original dimensions, working image path, and any error
 * message.
 */
auto generate_working_image(const std::filesystem::path &base_path,
                            const std::filesystem::path &src_path,
                            const std::filesystem::path &cache_path)
    -> ingestion_result;

/**
 * @brief Executes a folder ingestion pass, scanning the specified source
 * folder for image files, generating working images, and storing metadata in
 * the database.
 *
 * Emits `on_image` (after each working image is written) and
 * `progress_callback` (periodically) from scheduler threads. Reads EXIF
 * capture metadata for each successfully prepared image.
 *
 * @param db The database instance to store image metadata.
 * @param source_folder The path to the source folder to scan.
 * @param recursive Whether to descend into subfolders.
 * @param stop_token Cancellation token; remaining images are skipped once
 * requested.
 */
auto execute_folder_ingestion_pass(
    database &db, const std::filesystem::path &source_folder, bool recursive,
    std::stop_token stop_token,
    const ingestion_progress_callback &progress_callback,
    const ingestion_image_callback &on_image) -> ingestion_summary;

} // namespace kustavi::image
