#pragma once

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

#if !defined(_WIN32)
#include <pwd.h>
#include <unistd.h>
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
