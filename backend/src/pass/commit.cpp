#include "pass/commit.h"

#include "pass/related_files.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace kustavi {

namespace {

/** Where a companion of `primary` lands when the primary is written to
 * `primary_dest`: same directory, same (possibly suffixed) stem, and the rest
 * of the companion's name unchanged. */
auto companion_dest(const fs::path &primary, const fs::path &companion,
                    const fs::path &primary_dest) -> fs::path {
  const auto tail =
      companion.filename().string().substr(primary.stem().string().size());
  return primary_dest.parent_path() / (primary_dest.stem().string() + tail);
}

/** True when the suffixed primary name or any companion name is taken. */
auto any_exists(const fs::path &primary_dest, const fs::path &primary,
                const std::vector<fs::path> &companions) -> bool {
  std::error_code ec;
  if (fs::exists(primary_dest, ec) || ec) {
    return true;
  }
  return std::ranges::any_of(companions, [&](const fs::path &c) -> bool {
    std::error_code exists_ec;
    return fs::exists(companion_dest(primary, c, primary_dest), exists_ec) ||
           exists_ec;
  });
}

/** Copies each companion beside the primary. A failure is reported but never
 * undoes the primary copy. */
void copy_companions(const commit_source &source,
                     const std::vector<fs::path> &companions,
                     const fs::path &primary_dest, commit_summary &summary) {
  for (const auto &companion : companions) {
    const auto dest = companion_dest(source.path, companion, primary_dest);
    std::error_code ec;
    if (fs::exists(dest, ec) && !ec) {
      const auto dest_size = fs::file_size(dest, ec);
      const auto src_size = fs::file_size(companion, ec);
      if (!ec && dest_size == src_size) {
        summary.companions++;
        continue;
      }
      summary.errors.push_back(source.id + ": " +
                               companion.filename().string() +
                               ": name conflict");
      continue;
    }
    std::error_code copy_ec;
    fs::copy(companion, dest, fs::copy_options::none, copy_ec);
    if (copy_ec) {
      summary.errors.push_back(source.id + ": " +
                               companion.filename().string() + ": " +
                               copy_ec.message());
      continue;
    }
    spdlog::debug("committed companion '{}' -> '{}'", companion.string(),
                  dest.string());
    summary.companions++;
  }
}

} // namespace

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

    fs::path dest_path;
    if (source.dest_subdir.empty()) {
      std::error_code rel_ec;
      const auto relative = fs::relative(source.path, session_folder, rel_ec);
      if (rel_ec) {
        summary.errors.push_back(source.id + ": " + rel_ec.message());
        continue;
      }
      dest_path = destination / relative;
    } else {
      dest_path = destination / source.dest_subdir / source.path.filename();
    }

    std::error_code dir_ec;
    fs::create_directories(dest_path.parent_path(), dir_ec);
    if (dir_ec) {
      summary.errors.push_back(source.id + ": " + dir_ec.message());
      continue;
    }

    const auto companions = find_companions(source.path);

    std::error_code size_ec;
    if (fs::exists(dest_path, size_ec) && !size_ec) {
      const auto dest_size = fs::file_size(dest_path, size_ec);
      const auto src_size = fs::file_size(source.path, size_ec);
      if (!size_ec && dest_size == src_size) {
        // Same size: treat as already copied (idempotent re-commits).
        summary.copied++;
        copy_companions(source, companions, dest_path, summary);
        continue;
      }
      if (source.dest_subdir.empty()) {
        summary.skipped++;
        summary.errors.push_back(source.id + ": name conflict");
        continue;
      }
      // Trip-folder layout can legitimately collide two different files with
      // the same name (IMG_0001.jpg from two cameras): disambiguate. The
      // companions take the same suffix so the group stays aligned.
      const auto prefix = dest_path.stem().string() + "-";
      const auto ext = dest_path.extension().string();
      const auto dir = dest_path.parent_path();
      fs::path candidate;
      for (int n = 2;; ++n) {
        candidate = dir / (prefix + std::to_string(n) + ext);
        if (!any_exists(candidate, source.path, companions)) {
          break;
        }
      }
      dest_path = candidate;
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
    copy_companions(source, companions, dest_path, summary);
  }

  return summary;
}
} // namespace kustavi
