#include "similar.h"
#include "exec/scheduler.h"
#include "image_features.h"
#include "vantage_point_tree.h"

#include <opencv2/core.hpp>      // For cv::Mat, cv::Scalar, cv::meanStdDev
#include <opencv2/imgcodecs.hpp> // For cv::imread
#include <opencv2/imgproc.hpp>   // For cv::Laplacian, cv::calcHist
#include <spdlog/spdlog.h>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <bit>

namespace kustavi::image {

namespace ex = stdexec;

/** Computes a 64-bit Difference Hash (dHash) using core OpenCV */
auto compute_dhash(const cv::Mat &gray) -> uint64_t {
  cv::Mat resized;
  // 9x8 allows us to compare 8 horizontal pairs across 8 rows (64 bits)
  cv::resize(gray, resized, cv::Size(9, 8), 0, 0, cv::INTER_AREA);

  uint64_t hash = 0;
  int bit_index = 0;

  for (int row = 0; row < 8; ++row) {
    for (int col = 0; col < 8; ++col) {
      // Compare left pixel to right pixel
      bool bit = resized.at<uchar>(row, col) > resized.at<uchar>(row, col + 1);
      if (bit) {
        hash |= (1ULL << bit_index);
      }
      bit_index++;
    }
  }
  return hash;
}

/** Extract features from a single image using modern std::array wrappers */
auto extract_features(const std::filesystem::path &path) -> image_features {
  image_features feats{.path = path};

  cv::Mat img = cv::imread(path.string(), cv::IMREAD_COLOR);
  if (img.empty()) {
    return feats;
  }

  // 1. Compute Color Histogram (HS channels) using std::array
  cv::Mat hsv;
  cv::cvtColor(img, hsv, cv::COLOR_BGR2HSV);

  constexpr int h_bins = 50;
  constexpr int s_bins = 60;

  constexpr std::array<int, 2> hist_size{h_bins, s_bins};
  constexpr std::array<float, 2> h_ranges{0.0f, 180.0f};
  constexpr std::array<float, 2> s_ranges{0.0f, 256.0f};

  std::array<const float *, 2> ranges{h_ranges.data(), s_ranges.data()};
  constexpr std::array<int, 2> channels{0, 1};

  cv::calcHist(&hsv, 1, channels.data(), cv::Mat(), feats.histogram, 2,
               hist_size.data(), ranges.data(), true, false);

  cv::normalize(feats.histogram, feats.histogram, 0, 1, cv::NORM_MINMAX, -1,
                cv::Mat());

  // 2. Compute Custom dHash
  cv::Mat gray;
  cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
  feats.dhash = compute_dhash(gray);

  feats.valid = true;
  return feats;
}

auto find_similar_images(
    const double similarity_radius,
    const std::vector<std::filesystem::path> &paths,
    const std::function<void(std::size_t images_analyzed)> &progress_callback)
    -> std::vector<std::vector<std::filesystem::path>> {

  if (paths.empty()) {
    return {};
  }

  auto scheduler = exec::make_scheduler();

  std::vector<image_features> all_features(paths.size());
  std::atomic<std::size_t> completed_count{0};

  // 2. Define parallel bulk work using stdexec
  auto work_pipeline =
      ex::schedule(scheduler) |
      ex::bulk(ex::par, paths.size(), [&](std::size_t idx) -> void {
        all_features[idx] = extract_features(paths[idx]);

        if (progress_callback) {
          progress_callback(
              completed_count.fetch_add(1, std::memory_order_relaxed) + 1);
        }
      });

  // 3. Run the pipeline pipeline on the pool
  stdexec::sync_wait(work_pipeline);

  // 4. Sequential Clustering using Index Mapping
  std::vector<std::vector<std::filesystem::path>> groups;
  std::unique_ptr<collection::vp_node> root = nullptr;

  auto valid_features =
      all_features | std::views::filter(&image_features::valid);

  for (auto &feats : valid_features) {
    if (!root) {
      feats.cluster_idx = groups.size();
      groups.push_back({feats.path});
      root = std::make_unique<collection::vp_node>(feats);
      continue;
    }

    if (const auto *match = root->find_similar(feats, similarity_radius)) {
      groups[match->cluster_idx].push_back(feats.path);
    } else {
      feats.cluster_idx = groups.size();
      groups.push_back({feats.path});
      root->insert(feats);
    }
  }

  return groups;
}
} // namespace kustavi::image
