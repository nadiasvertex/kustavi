#pragma once

#include <filesystem>
#include <functional>
#include <vector>

namespace kustavi::image {

/** Default similarity radius: the normalized hash distance below which two
 * images count as near-duplicates. */
inline constexpr double default_similarity_radius = 0.15;

/**
 * Find similar images in a batch of image paths.
 */
auto find_similar_images(
    const double similarity_radius,
    const std::vector<std::filesystem::path> &paths,
    const std::function<void(std::size_t images_analyzed)> &progress_callback)
    -> std::vector<std::vector<std::filesystem::path>>;
} // namespace kustavi::image
