#pragma once

#include "core/domain.h"

#include <chrono>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kustavi::gui {

// How close in time two photos must be to count as neighbors.
inline constexpr auto neighbor_window = std::chrono::minutes{2};

// Finds the photos taken next to each flagged photo, so a reviewer can see whether a better shot
// of the same moment exists.
//
// Photos are ordered by capture time. Photos without a time and videos are left out. Each flagged
// photo gets up to `per_side` photos before it and after it that are within `neighbor_window` and
// are not in `excluded`. A flagged photo with no neighbors has no entry.
[[nodiscard]] auto find_neighbors(std::vector<image_info> const &candidates,
                                  std::unordered_set<std::string> const &flagged_ids,
                                  std::unordered_set<std::string> const &excluded = {},
                                  int per_side = 2)
    -> std::unordered_map<std::string, std::vector<image_info>>;

} // namespace kustavi::gui
