#pragma once

#include <opencv2/core.hpp>

#include <cstdint>
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

/** 64-bit difference hash (dHash) of a grayscale image. Shared with the video
 * pass, which hashes sampled frames to detect motion between them. */
auto compute_dhash(const cv::Mat &gray) -> uint64_t;
} // namespace kustavi::image
