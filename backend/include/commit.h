#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace kustavi {

/** A file to copy: an image id plus its absolute source path. */
struct commit_source {
  std::string id;
  std::filesystem::path path;
};

/** Outcome of a commit run. */
struct commit_summary {
  std::size_t copied = 0;
  std::size_t skipped = 0;
  std::vector<std::string> errors; //! "<id>: <reason>" per failure.
};

/** Copies `sources` into `destination`, preserving each file's path relative
 * to `session_folder` (subdirectories included).
 *
 * Collision policy: an existing destination file with the same size is
 * counted as copied (idempotent re-commits); a different size is skipped and
 * reported. Copy failures are reported per file and do not abort the run.
 * The session folder is never modified.
 */
auto commit_files(
    const std::filesystem::path &session_folder,
    const std::filesystem::path &destination,
    const std::vector<commit_source> &sources,
    const std::stop_token &stop_token,
    const std::function<void(std::size_t done, std::size_t total,
                             const std::filesystem::path &current)>
        &progress_callback) -> commit_summary;
} // namespace kustavi
