#pragma once

#include "database.h"

#include <filesystem>
#include <functional>
#include <string>

namespace kustavi::image {

/**
 * @brief Represents the result of an image ingestion operation.
 */
struct ingestion_result {
  bool success = false;    //! Indicates whether the ingestion was successful.
  int original_width = 0;  //! The original width of the source image.
  int original_height = 0; //! The original height of the source image.
  std::string working_image_path; //! The path to the generated working image in
                                  //! the cache.
  std::string error_message;      //! An error message describing the reason for
                                  //! failure, if any.
};

/**
 * @brief Generates a working image from the source image and stores it in the
 * cache directory.
 *
 * @param src_path The path to the source image.
 * @param cache_path The path to the cache directory where the working image
 * will be stored.
 * @return ingestion_result The result of the ingestion operation, including
 * success status, original dimensions, working image path, and any error
 * message.
 */
auto generate_working_image(const std::filesystem::path &src_path,
                            const std::filesystem::path &cache_path)
    -> ingestion_result;

/**
 * @brief Executes a folder ingestion pass, scanning the specified source
 * folder for image files, generating working images, and storing metadata in
 * the database.
 *
 * @param db The database instance to store image metadata.
 * @param source_folder The path to the source folder to scan for image files.
 * @param progress_callback A callback function that receives the number of
 * files seen and the number of images found during the ingestion process.
 *
 */
void execute_folder_ingestion_pass(
    database &db, const std::string &source_folder,
    const std::function<void(int files_seen, int images_found)>
        &progress_callback);

} // namespace kustavi::image
