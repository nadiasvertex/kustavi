#pragma once

#include "image_features.h"

#include <opencv2/imgproc.hpp> // For cv::Laplacian, cv::calcHist

namespace kustavi::collection {
/** Immutable vantage point tree */
class vp_node {
public:
  image::image_features vantage_point;
  double threshold = 0.0;
  std::unique_ptr<vp_node> left;
  std::unique_ptr<vp_node> right;

  explicit vp_node(image::image_features feats)
      : vantage_point(std::move(feats)) {}

  [[nodiscard]] auto find_similar(const image::image_features &target,
                                  double max_radius) const
      -> const image::image_features * {
    double dist = calculate_distance(vantage_point, target);

    if (dist <= max_radius) {
      return &vantage_point;
    }

    if (dist - max_radius <= threshold && left) {
      if (auto found = left->find_similar(target, max_radius)) {
        return found;
      }
    }
    if (dist + max_radius >= threshold && right) {
      if (auto found = right->find_similar(target, max_radius)) {
        return found;
      }
    }

    return nullptr;
  }

  /**
   * Inserts a new image_features into the vantage point tree. If the tree is
   * empty, it initializes the root with the given target. Otherwise, it
   * calculates the distance from the target to the current node's vantage point
   * and decides whether to insert it into the left or right subtree based on
   * the threshold.
   */
  void insert(const image::image_features &target) {
    double dist = calculate_distance(vantage_point, target);

    if (threshold == 0.0) {
      threshold = dist;
    }

    if (dist <= threshold) {
      if (!left) {
        left = std::make_unique<vp_node>(target);
      } else {
        left->insert(target);
      }
    } else {
      if (!right) {
        right = std::make_unique<vp_node>(target);
      } else {
        right->insert(target);
      }
    }
  }
};

} // namespace kustavi::collection
