#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
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

/** Commit behavior switches. */
struct commit_options {
  /** Treat `destination` as an existing library: skip items whose content
   * already exists anywhere under it, and reuse existing folders (matched
   * case-insensitively) for `dest_subdir` components. */
  bool merge_existing = false;
};

/** Outcome of a commit run. */
struct commit_summary {
  std::size_t copied = 0;
  std::size_t already_present = 0; //! Skipped: identical file already in the library.
  std::size_t skipped = 0;
  std::size_t companions = 0; //! Related files copied with their primary.
  std::vector<std::string> errors; //! "<id>: <reason>" per failure.
};

/** Copies `sources` into `destination`, each with its related files (RAW
 * originals, Live Photo clips, sidecars; see `find_companions`). Companions
 * land beside the primary and share its final name, including a `-<n>`
 * collision suffix. Each file's path relative to `session_folder` is
 * preserved, unless its `dest_subdir` is set (trip/leg folder layout).
 *
 * Collision policy: an existing destination file with the same size is
 * counted as copied (idempotent re-commits). A different size in the
 * source-tree layout is skipped and reported; in the `dest_subdir` layout it
 * is instead written under a `-<n>` suffix, since distinct files sharing a
 * name in one folder is expected there. Copy failures are reported per file
 * and do not abort the run. The session folder is never modified.
 *
 * With `options.merge_existing`, an item whose bytes are identical to a file
 * already under `destination` is counted in `already_present` and not copied.
 */
auto commit_files(
    const std::filesystem::path &session_folder,
    const std::filesystem::path &destination,
    const std::vector<commit_source> &sources,
    const std::stop_token &stop_token,
    const std::function<void(std::size_t done, std::size_t total,
                             const std::filesystem::path &current)>
        &progress_callback,
    const commit_options &options = {}) -> commit_summary;

/** What a commit would write, and the room available for it. */
struct commit_estimate {
  std::uint64_t total_bytes = 0; //! Every kept file with its related files.
  std::uint64_t new_bytes = 0;   //! Bytes a commit would actually write.
  std::size_t already_present = 0; //! Items a commit would not copy.
  std::optional<std::uint64_t> free_bytes; //! Unset when it cannot be read.
};

/** Sizes a commit without writing anything. Items already at their
 * destination path with the same size, or (with `merge_existing`) identical
 * to a library file, count towards `already_present` and not `new_bytes`.
 * `free_bytes` is the space available to the nearest existing ancestor of
 * `destination`. */
auto estimate_commit(const std::filesystem::path &session_folder,
                     const std::filesystem::path &destination,
                     const std::vector<commit_source> &sources,
                     const commit_options &options = {}) -> commit_estimate;
} // namespace kustavi
