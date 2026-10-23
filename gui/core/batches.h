#pragma once

#include "core/deletion.h"
#include "core/domain.h"
#include "core/wizard_step.h"

#include <set>
#include <string_view>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kustavi::gui {

// The key of the batch for photos that belong to no trip.
inline constexpr auto unassigned_batch_key = std::string_view{"__unassigned__"};
// The key of the batch for a folder with no name.
inline constexpr auto no_folder_batch_key = std::string_view{"__no-folder__"};

// One batch for each trip folder that has photos, then one for the photos in no trip. A photo is
// in the batch order of `ordered_ids`.
[[nodiscard]] auto build_batches(std::vector<trip_folder_info> const &folders,
                                 std::vector<std::string> const &ordered_ids,
                                 std::vector<std::string> const &unassigned_ids)
    -> std::vector<batch_info>;

// Which passes finished. A pass that ran over the whole session counts for every batch.
struct pass_progress {
  // Wizard step indexes of passes that finished over the whole library.
  std::set<int> session;
  // "<step index>:<batch key>", the form that the back end reports.
  std::set<std::string> batches;

  [[nodiscard]] auto done(wizard_step step, std::string const &batch_key) const -> bool;
  auto mark_done(wizard_step step, std::string const &batch_key) -> void;
};

// The row of the batch menu for `batch`.
[[nodiscard]] auto summarize_batch(batch_info const &batch,
                                   std::unordered_map<std::string, image_info> const &images,
                                   review_flags const &flags, pass_progress const &progress)
    -> batch_summary;

} // namespace kustavi::gui
