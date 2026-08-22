#pragma once

#include <filesystem>
#include <functional>
#include <vector>

namespace kustavi::image {

/**
 * Find similar images in a batch of image paths.
 */
auto find_similar_images(
    const double similarity_radius,
    const std::vector<std::filesystem::path> &paths,
    const std::function<void(std::size_t images_analyzed)> &progress_callback)
    -> std::vector<std::vector<std::filesystem::path>>;
} // namespace kustavi::image
