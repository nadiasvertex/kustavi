#pragma once

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#if !defined(_WIN32)
#include <pwd.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

namespace kustavi::config {

namespace detail {

/** `std::getenv` as an optional owned string (empty env var reads as unset). */
inline auto env(const char *name) -> std::optional<std::string> {
  const char *value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  return std::string(value);
}

/** The user's home directory: `$HOME`, or the passwd entry when it is unset
 * (e.g. under `bazel run`, which scrubs the environment). */
inline auto home_dir() -> std::optional<std::string> {
  if (const auto value = env("HOME")) {
    return value;
  }
#if !defined(_WIN32)
  if (const passwd *pw = ::getpwuid(::getuid());
      pw != nullptr && pw->pw_dir != nullptr && *pw->pw_dir != '\0') {
    return std::string(pw->pw_dir);
  }
#endif
  return std::nullopt;
}

/** Directory containing the running executable, when the platform can report
 * it. Used to locate assets bundled next to the binary (packaged app) or in
 * the Bazel runfiles tree (`bazel run`). */
inline auto executable_dir() -> std::optional<std::filesystem::path> {
#if defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buf(size, '\0');
  if (_NSGetExecutablePath(buf.data(), &size) != 0) {
    return std::nullopt;
  }
  std::error_code ec;
  const auto canonical = std::filesystem::canonical(buf, ec);
  const std::filesystem::path exe = ec ? std::filesystem::path(buf) : canonical;
  return exe.parent_path();
#elif defined(_WIN32)
  std::wstring buf(MAX_PATH, L'\0');
  const DWORD len =
      ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
  if (len == 0) {
    return std::nullopt;
  }
  buf.resize(len);
  return std::filesystem::path(buf).parent_path();
#else
  std::error_code ec;
  const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec) {
    return std::nullopt;
  }
  return exe.parent_path();
#endif
}

} // namespace detail

/**
 * @brief Per-user application data directory for assets that outlive a single
 * session (currently the vision model weights).
 *
 * macOS: `~/Library/Application Support/Kustavi`
 * Linux: `${XDG_DATA_HOME:-~/.local/share}/kustavi`
 * Windows: `%LOCALAPPDATA%\Kustavi`
 */
inline auto app_data_path() -> std::filesystem::path {
#if defined(__APPLE__)
  if (const auto home = detail::home_dir()) {
    return std::filesystem::path(*home) / "Library" / "Application Support" /
           "Kustavi";
  }
  return std::filesystem::current_path() / ".kustavi";
#elif defined(_WIN32)
  if (const auto local = detail::env("LOCALAPPDATA")) {
    return std::filesystem::path(*local) / "Kustavi";
  }
  return std::filesystem::current_path() / ".kustavi";
#else
  if (const auto xdg = detail::env("XDG_DATA_HOME")) {
    return std::filesystem::path(*xdg) / "kustavi";
  }
  if (const auto home = detail::home_dir()) {
    return std::filesystem::path(*home) / ".local" / "share" / "kustavi";
  }
  return std::filesystem::current_path() / ".kustavi";
#endif
}

/** Directory holding the downloaded vision-model GGUF files. */
inline auto models_path() -> std::filesystem::path {
  return app_data_path() / "models";
}

/**
 * @brief Locates the bundled GeoNames place table used by the trips pass to
 * reverse-geocode trip centroids into folder names ("Rome, Italy").
 *
 * Search order: `$KUSTAVI_GEO_DATA`, then next to the executable (packaged
 * app), then the Bazel runfiles layout (`bazel run` / smoke client), then a
 * workspace-relative path (developer shell), then the app-data directory.
 * Returns an empty path when no table is found; the trips pass then falls
 * back to month-year folder names.
 */
inline auto geo_data_path() -> std::filesystem::path {
  namespace fs = std::filesystem;
  if (const auto override_path = detail::env("KUSTAVI_GEO_DATA")) {
    return {*override_path};
  }

  std::vector<fs::path> candidates;
  if (const auto exe = detail::executable_dir()) {
    candidates.push_back(*exe / "cities.tsv");
    candidates.push_back(*exe / "data" / "cities.tsv");
    candidates.push_back(*exe / "backend" / "data" / "cities.tsv");
  }
  candidates.push_back(fs::path("backend") / "data" / "cities.tsv");
  candidates.push_back(fs::path("data") / "cities.tsv");
  candidates.push_back(app_data_path() / "geo" / "cities.tsv");

  for (const auto &candidate : candidates) {
    std::error_code ec;
    if (fs::exists(candidate, ec) && !ec) {
      return candidate;
    }
  }
  return {};
}

namespace detail {

/**
 * Locate a small data file bundled next to the executable / in the Bazel
 * runfiles tree / in a workspace-relative `backend/data` directory. Mirrors
 * the search order of `geo_data_path`. `env_override`, when set and non-empty,
 * wins outright. Returns an empty path when nothing is found.
 */
inline auto bundled_data_path(const char *filename, const char *env_override)
    -> std::filesystem::path {
  namespace fs = std::filesystem;
  if (const auto override_path = env(env_override)) {
    return {*override_path};
  }

  std::vector<fs::path> candidates;
  if (const auto exe = executable_dir()) {
    candidates.push_back(*exe / filename);
    candidates.push_back(*exe / "data" / filename);
    candidates.push_back(*exe / "backend" / "data" / filename);
  }
  candidates.push_back(fs::path("backend") / "data" / filename);
  candidates.push_back(fs::path("data") / filename);
  candidates.push_back(app_data_path() / "models" / filename);

  for (const auto &candidate : candidates) {
    std::error_code ec;
    if (fs::exists(candidate, ec) && !ec) {
      return candidate;
    }
  }
  return {};
}

} // namespace detail

/**
 * @brief Locates the bundled YuNet face-detector ONNX model used by the
 * similar pass to score keeper "bestness" by face quality (faces present and
 * in focus, eyes open, no red-eye). Search order matches `geo_data_path`;
 * `$KUSTAVI_FACE_MODEL` overrides. Empty when absent (the similar pass then
 * falls back to sharpness + color-balance scoring only).
 */
inline auto face_model_path() -> std::filesystem::path {
  return detail::bundled_data_path("face_detection_yunet.onnx",
                                   "KUSTAVI_FACE_MODEL");
}

/**
 * @brief Returns the path to the Kustavi cache directory based on the provided
 * base directory.
 */
inline auto cache_path(const std::filesystem::path &base_dir)
    -> std::filesystem::path {
  return base_dir / ".kustavi-cache";
}

/**
 * @brief Returns the path to the image cache directory within the Kustavi
 * cache.
 */
inline auto image_cache_path(const std::filesystem::path &cache_dir)
    -> std::filesystem::path {
  return cache_dir / "res-768";
}

/**
 * @brief Returns the path to the session database path within the Kustavi
 * cache.
 */
inline auto session_db_path(const std::filesystem::path &cache_dir)
    -> std::filesystem::path {
  return cache_dir / "session.db";
}

} // namespace kustavi::config
