#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace kustavi {

/** A file to copy: an image id plus its absolute source path.
 *
 * When `dest_subdir` is set, the file lands at
 * `<destination>/<dest_subdir>/<filename>` (trip/leg folder layout);
 * otherwise its path relative to the session folder is preserved. */
struct commit_source {
  std::string id;
  std::filesystem::path path;
  std::filesystem::path dest_subdir; //! Relative; empty = preserve source tree.
};

/** Outcome of a commit run. */
struct commit_summary {
  std::size_t copied = 0;
  std::size_t skipped = 0;
  std::vector<std::string> errors; //! "<id>: <reason>" per failure.
};

/** Copies `sources` into `destination`. Each file's path relative to
 * `session_folder` is preserved, unless its `dest_subdir` is set (trip/leg
 * folder layout).
 *
 * Collision policy: an existing destination file with the same size is
 * counted as copied (idempotent re-commits). A different size in the
 * source-tree layout is skipped and reported; in the `dest_subdir` layout it
 * is instead written under a `-<n>` suffix, since distinct files sharing a
 * name in one folder is expected there. Copy failures are reported per file
 * and do not abort the run. The session folder is never modified.
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
