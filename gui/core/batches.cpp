#include "core/batches.h"

#include <algorithm>

namespace kustavi::gui {

auto build_batches(std::vector<trip_folder_info> const &folders,
                   std::vector<std::string> const &ordered_ids,
                   std::vector<std::string> const &unassigned_ids) -> std::vector<batch_info> {
  std::vector<batch_info> batches;
  for (auto const &folder : folders) {
    std::unordered_set<std::string> members;
    for (auto const &trip : folder.trips) {
      members.insert(trip.member_ids.begin(), trip.member_ids.end());
    }
    if (members.empty()) {
      continue;
    }
    batch_info batch;
    batch.key = folder.name.empty() ? std::string{no_folder_batch_key} : folder.name;
    batch.title = folder.name.empty() ? "Other" : folder.name;
    std::ranges::copy_if(ordered_ids, std::back_inserter(batch.image_ids),
                         [&](std::string const &id) { return members.contains(id); });
    batches.push_back(std::move(batch));
  }
  if (!unassigned_ids.empty()) {
    batches.push_back({std::string{unassigned_batch_key}, "Unassigned", unassigned_ids});
  }
  return batches;
}

auto pass_progress::done(wizard_step step, std::string const &batch_key) const -> bool {
  return session.contains(index_of(step)) ||
         batches.contains(std::to_string(index_of(step)) + ":" + batch_key);
}

auto pass_progress::mark_done(wizard_step step, std::string const &batch_key) -> void {
  batches.insert(std::to_string(index_of(step)) + ":" + batch_key);
}

auto summarize_batch(batch_info const &batch,
                     std::unordered_map<std::string, image_info> const &images,
                     review_flags const &flags, pass_progress const &progress) -> batch_summary {
  std::unordered_set<std::string> const ids{batch.image_ids.begin(), batch.image_ids.end()};
  int videos = 0;
  for (auto const &id : ids) {
    auto const image = images.find(id);
    if (image != images.end() && image->second.is_video) {
      ++videos;
    }
  }
  auto const flagged = [&](auto const &map) {
    return static_cast<int>(std::ranges::count_if(
        map, [&](auto const &entry) { return ids.contains(entry.first); }));
  };
  auto const groups = static_cast<int>(std::ranges::count_if(
      flags.similar_groups, [&](similar_group_info const &group) {
        return std::ranges::any_of(group.member_ids,
                                   [&](std::string const &id) { return ids.contains(id); });
      }));
  auto const status = [&](wizard_step step, int count) {
    return std::pair{index_of(step), batch_pass_status{progress.done(step, batch.key), count}};
  };

  batch_summary summary;
  summary.key = batch.key;
  summary.title = batch.title;
  summary.photo_count = static_cast<int>(ids.size()) - videos;
  summary.video_count = videos;
  summary.passes.insert({status(wizard_step::quality, flagged(flags.quality)),
                         status(wizard_step::duplicates, groups),
                         status(wizard_step::junk, flagged(flags.junk)),
                         status(wizard_step::video, flagged(flags.video))});
  return summary;
}

} // namespace kustavi::gui
