#pragma once

#include <filesystem>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace kustavi::image {

/** Holds precomputed image features */
struct image_features {
  std::filesystem::path path;
  cv::Mat histogram;
  uint64_t dhash = 0;
  size_t cluster_idx = 0; // Tracks target bucket index inside final vector
  bool valid = false;
};

/** Computes the Hamming distance between two 64-bit integers */
inline auto compute_hamming_distance(uint64_t x, uint64_t y) -> int {
  return std::popcount(x ^ y);
}

/** Calculate joint structural/color distance between images */
inline auto calculate_distance(const image_features &a, const image_features &b)
    -> double {
  if (!a.valid || !b.valid) {
    return std::numeric_limits<double>::max();
  }

  double corr = cv::compareHist(a.histogram, b.histogram, cv::HISTCMP_CORREL);
  double hist_dist = 1.0 - std::clamp(corr, -1.0, 1.0);

  double hamming = compute_hamming_distance(a.dhash, b.dhash);
  double hash_dist = hamming / 64.0;

  return (0.4 * hist_dist) + (0.6 * hash_dist);
}

} // namespace kustavi::image
