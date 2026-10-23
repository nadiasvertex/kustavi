#include "core/neighbors.h"

#include <algorithm>

namespace kustavi::gui {

auto find_neighbors(std::vector<image_info> const &candidates,
                    std::unordered_set<std::string> const &flagged_ids,
                    std::unordered_set<std::string> const &excluded, int per_side)
    -> std::unordered_map<std::string, std::vector<image_info>> {
  std::vector<image_info const *> timed;
  for (auto const &image : candidates) {
    if (image.taken && !image.is_video) {
      timed.push_back(&image);
    }
  }
  std::ranges::stable_sort(
      timed, [](auto const *a, auto const *b) { return *a->taken < *b->taken; });

  std::unordered_map<std::string, std::vector<image_info>> result;
  for (std::size_t i = 0; i < timed.size(); ++i) {
    image_info const &image = *timed[i];
    if (!flagged_ids.contains(image.id)) {
      continue;
    }
    std::vector<image_info> found;
    int taken = 0;
    for (std::size_t j = i; j-- > 0 && taken < per_side;) {
      if (*image.taken - *timed[j]->taken > neighbor_window) {
        break;
      }
      if (!excluded.contains(timed[j]->id)) {
        found.insert(found.begin(), *timed[j]);
        ++taken;
      }
    }
    taken = 0;
    for (std::size_t j = i + 1; j < timed.size() && taken < per_side; ++j) {
      if (*timed[j]->taken - *image.taken > neighbor_window) {
        break;
      }
      if (!excluded.contains(timed[j]->id)) {
        found.push_back(*timed[j]);
        ++taken;
      }
    }
    if (!found.empty()) {
      result[image.id] = std::move(found);
    }
  }
  return result;
}

} // namespace kustavi::gui
