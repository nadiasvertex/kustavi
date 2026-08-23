#include "commit.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace kustavi {

auto commit_files(
    const std::filesystem::path &session_folder,
    const std::filesystem::path &destination,
    const std::vector<commit_source> &sources,
    const std::stop_token &stop_token,
    const std::function<void(std::size_t done, std::size_t total,
                             const std::filesystem::path &current)>
        &progress_callback) -> commit_summary {
  commit_summary summary;

  std::error_code ec;
  fs::create_directories(destination, ec);
  if (ec) {
    summary.errors.push_back("destination: " + ec.message());
    return summary;
  }

  for (std::size_t i = 0; i < sources.size(); ++i) {
    const auto &source = sources[i];

    if (stop_token.stop_requested()) {
      break;
    }

    if (progress_callback) {
      progress_callback(i + 1, sources.size(), source.path);
    }

    std::error_code rel_ec;
    const auto relative = fs::relative(source.path, session_folder, rel_ec);
    if (rel_ec) {
      summary.errors.push_back(source.id + ": " + rel_ec.message());
      continue;
    }

    const auto dest_path = destination / relative;
    std::error_code dir_ec;
    fs::create_directories(dest_path.parent_path(), dir_ec);
    if (dir_ec) {
      summary.errors.push_back(source.id + ": " + dir_ec.message());
      continue;
    }

    std::error_code size_ec;
    if (fs::exists(dest_path, size_ec) && !size_ec) {
      const auto dest_size = fs::file_size(dest_path, size_ec);
      const auto src_size = fs::file_size(source.path, size_ec);
      if (!size_ec && dest_size == src_size) {
        // Same size: treat as already copied (idempotent re-commits).
        summary.copied++;
        continue;
      }
      summary.skipped++;
      summary.errors.push_back(source.id + ": name conflict");
      continue;
    }

    std::error_code copy_ec;
    try {
      fs::copy(source.path, dest_path, fs::copy_options::none, copy_ec);
    } catch (const std::exception &e) {
      summary.errors.push_back(source.id + ": " + e.what());
      continue;
    }
    if (copy_ec) {
      summary.errors.push_back(source.id + ": " + copy_ec.message());
      continue;
    }

    spdlog::debug("committed '{}' -> '{}'", source.path.string(),
                  dest_path.string());
    summary.copied++;
  }

  return summary;
}
} // namespace kustavi
