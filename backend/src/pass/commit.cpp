#include "pass/commit.h"

#include "pass/related_files.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <system_error>
#include <unordered_map>

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

using size_index = std::unordered_map<std::uintmax_t, std::vector<fs::path>>;

auto lowered(std::string text) -> std::string {
  std::ranges::transform(text, text.begin(), [](unsigned char c) -> char {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

/** Indexes every regular file under `root` by size. Unreadable entries are
 * skipped. */
auto build_library_index(const fs::path &root) -> size_index {
  size_index index;
  std::error_code ec;
  if (!fs::is_directory(root, ec)) {
    return index;
  }
  fs::recursive_directory_iterator it(
      root, fs::directory_options::skip_permission_denied, ec);
  for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    std::error_code entry_ec;
    if (!it->is_regular_file(entry_ec) || entry_ec) {
      continue;
    }
    const auto size = it->file_size(entry_ec);
    if (!entry_ec) {
      index[size].push_back(it->path());
    }
  }
  return index;
}

/** Byte-for-byte comparison of two files. */
auto files_identical(const fs::path &a, const fs::path &b) -> bool {
  std::ifstream fa(a, std::ios::binary);
  std::ifstream fb(b, std::ios::binary);
  if (!fa || !fb) {
    return false;
  }
  constexpr std::size_t chunk = 64 * 1024;
  std::vector<char> buf_a(chunk);
  std::vector<char> buf_b(chunk);
  while (fa && fb) {
    fa.read(buf_a.data(), static_cast<std::streamsize>(chunk));
    fb.read(buf_b.data(), static_cast<std::streamsize>(chunk));
    if (fa.gcount() != fb.gcount() ||
        !std::equal(buf_a.begin(), buf_a.begin() + fa.gcount(),
                    buf_b.begin())) {
      return false;
    }
  }
  return fa.eof() == fb.eof();
}

/** True when `file` has a byte-identical twin in `index`. */
auto in_library(const size_index &index, const fs::path &file) -> bool {
  std::error_code ec;
  const auto size = fs::file_size(file, ec);
  if (ec) {
    return false;
  }
  const auto it = index.find(size);
  return it != index.end() &&
         std::ranges::any_of(it->second, [&](const fs::path &twin) -> bool {
           return files_identical(file, twin);
         });
}

/** Rewrites `subdir` so each component reuses an existing directory under
 * `destination` that differs only in letter case ("Italy-2019-07" for
 * "italy-2019-07"). Components with no match are kept as given. */
auto match_existing_dirs(const fs::path &destination, const fs::path &subdir)
    -> fs::path {
  fs::path current = destination;
  fs::path result;
  bool searching = true;
  for (const auto &part : subdir) {
    fs::path chosen = part;
    if (searching) {
      std::error_code ec;
      bool found = false;
      if (fs::exists(current / part, ec) && !ec) {
        found = true;
      } else {
        fs::directory_iterator it(current, ec);
        for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
          std::error_code dir_ec;
          if (it->is_directory(dir_ec) &&
              lowered(it->path().filename().string()) ==
                  lowered(part.string())) {
            chosen = it->path().filename();
            found = true;
            break;
          }
        }
      }
      searching = found;
    }
    result /= chosen;
    current /= chosen;
  }
  return result;
}

/** Where `source` lands under `destination`, or the reason it has no place. */
auto destination_path(const fs::path &session_folder,
                      const fs::path &destination, const commit_source &source,
                      bool merge_existing) -> std::expected<fs::path, std::string> {
  if (source.dest_subdir.empty()) {
    std::error_code rel_ec;
    const auto relative = fs::relative(source.path, session_folder, rel_ec);
    if (rel_ec) {
      return std::unexpected(rel_ec.message());
    }
    return destination / relative;
  }
  const auto subdir = merge_existing
                          ? match_existing_dirs(destination, source.dest_subdir)
                          : source.dest_subdir;
  return destination / subdir / source.path.filename();
}

auto same_size(const fs::path &a, const fs::path &b) -> bool {
  std::error_code ec;
  const auto size_a = fs::file_size(a, ec);
  if (ec) {
    return false;
  }
  const auto size_b = fs::file_size(b, ec);
  return !ec && size_a == size_b;
}

auto file_bytes(const fs::path &path) -> std::uint64_t {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  return ec ? 0 : size;
}

auto item_bytes(const fs::path &primary,
                const std::vector<fs::path> &companions) -> std::uint64_t {
  return std::ranges::fold_left(
      companions, file_bytes(primary),
      [](std::uint64_t sum, const fs::path &c) -> std::uint64_t {
        return sum + file_bytes(c);
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
        &progress_callback,
    const commit_options &options) -> commit_summary {
  commit_summary summary;

  std::error_code ec;
  fs::create_directories(destination, ec);
  if (ec) {
    summary.errors.push_back("destination: " + ec.message());
    return summary;
  }

  const auto library =
      options.merge_existing ? build_library_index(destination) : size_index{};

  for (std::size_t i = 0; i < sources.size(); ++i) {
    const auto &source = sources[i];

    if (stop_token.stop_requested()) {
      break;
    }

    if (progress_callback) {
      progress_callback(i + 1, sources.size(), source.path);
    }

    auto planned = destination_path(session_folder, destination, source,
                                    options.merge_existing);
    if (!planned) {
      summary.errors.push_back(source.id + ": " + planned.error());
      continue;
    }
    fs::path dest_path = *std::move(planned);
    const auto companions = find_companions(source.path);

    std::error_code exists_ec;
    const bool dest_taken = fs::exists(dest_path, exists_ec) && !exists_ec;
    if (dest_taken && same_size(dest_path, source.path)) {
      // Same size: treat as already copied (idempotent re-commits).
      summary.copied++;
      copy_companions(source, companions, dest_path, summary);
      continue;
    }
    if (options.merge_existing && in_library(library, source.path)) {
      summary.already_present++;
      continue;
    }

    std::error_code dir_ec;
    fs::create_directories(dest_path.parent_path(), dir_ec);
    if (dir_ec) {
      summary.errors.push_back(source.id + ": " + dir_ec.message());
      continue;
    }

    if (dest_taken) {
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

auto estimate_commit(const fs::path &session_folder, const fs::path &destination,
                     const std::vector<commit_source> &sources,
                     const commit_options &options) -> commit_estimate {
  commit_estimate estimate;
  const auto library =
      options.merge_existing ? build_library_index(destination) : size_index{};

  for (const auto &source : sources) {
    const auto companions = find_companions(source.path);
    const auto bytes = item_bytes(source.path, companions);
    estimate.total_bytes += bytes;

    const auto planned = destination_path(session_folder, destination, source,
                                          options.merge_existing);
    std::error_code ec;
    const bool present =
        (planned && fs::exists(*planned, ec) && !ec &&
         same_size(*planned, source.path)) ||
        (options.merge_existing && in_library(library, source.path));
    if (present) {
      estimate.already_present++;
    } else {
      estimate.new_bytes += bytes;
    }
  }

  // Free space comes from the nearest ancestor that exists; the destination
  // itself is created by the commit.
  std::error_code ec;
  fs::path probe = destination;
  while (!probe.empty() && !fs::exists(probe, ec)) {
    const auto parent = probe.parent_path();
    if (parent == probe) {
      break;
    }
    probe = parent;
  }
  const auto space = fs::space(probe, ec);
  if (!ec) {
    estimate.free_bytes = space.available;
  }
  return estimate;
}

} // namespace kustavi
