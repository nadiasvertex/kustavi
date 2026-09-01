#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>

namespace kustavi::net {

/** Byte counters for one in-flight asset download. */
struct download_progress {
  std::uint64_t done_bytes = 0;
  std::uint64_t total_bytes = 0; //! 0 until the server reports a size.
  double speed_bps = 0.0;        //! Rolling-window transfer rate.
};

using download_progress_cb = std::function<void(const download_progress &)>;

/** A file to fetch, with its expected size and content hash. */
struct remote_asset {
  std::string url;
  std::filesystem::path dest;
  std::string sha256_hex; //! Lowercase hex; verified after download.
  std::uint64_t size_bytes = 0;
};

/** True when `dest` already exists with the expected size and SHA-256. */
[[nodiscard]] auto asset_ready(const remote_asset &asset) -> bool;

/**
 * @brief Downloads `asset` to `asset.dest`, verifying its SHA-256.
 *
 * Writes to `<dest>.part` and renames on success. A pre-existing `.part` is
 * resumed via an HTTP Range request. `progress` is invoked (from this thread)
 * roughly twice a second. A requested `stop_token` aborts the transfer within
 * ~1 second, leaving the `.part` file in place for a later resume.
 *
 * @return nothing on success, or a human-readable error string.
 */
[[nodiscard]] auto download_asset(const remote_asset &asset,
                                  std::stop_token stop_token,
                                  const download_progress_cb &progress)
    -> std::expected<void, std::string>;

} // namespace kustavi::net
