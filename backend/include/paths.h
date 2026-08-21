#include <filesystem>

namespace kustavi::config {

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
