#include "core/deletion.h"

#include "core/domain.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>

namespace kustavi::gui {

namespace {

auto with_added(std::set<std::string> values, std::string const &id) -> std::set<std::string> {
  values.insert(id);
  return values;
}

auto without(std::set<std::string> values, std::string const &id) -> std::set<std::string> {
  values.erase(id);
  return values;
}

auto without_any(std::set<std::string> values, std::set<std::string> const &others)
    -> std::set<std::string> {
  std::erase_if(values, [&](std::string const &v) { return others.contains(v); });
  return values;
}

template <class Predicate>
auto filtered(std::vector<std::string> const &ids, Predicate &&keep) -> std::vector<std::string> {
  std::vector<std::string> out;
  std::ranges::copy_if(ids, std::back_inserter(out), keep);
  return out;
}

auto capitalized(std::string text) -> std::string {
  if (!text.empty()) {
    text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
  }
  return text;
}

} // namespace

auto deletion_intent::user_keeper_of(std::uint32_t group_id) const -> std::optional<std::string> {
  auto const found = group_keepers.find(group_id);
  return found == group_keepers.end() ? std::nullopt : std::optional{found->second};
}

auto deletion_intent::with_toggle(std::string const &id) const -> deletion_intent {
  return is_explicitly_deleted(id) ? with_kept(id) : with_marked(id);
}

auto deletion_intent::with_kept(std::string const &id) const -> deletion_intent {
  return {with_added(explicit_kept, id), without(explicit_deleted, id), group_keepers};
}

auto deletion_intent::with_marked(std::string const &id) const -> deletion_intent {
  return {without(explicit_kept, id), with_added(explicit_deleted, id), group_keepers};
}

auto deletion_intent::with_keep_all(std::vector<std::string> const &ids) const -> deletion_intent {
  auto kept = explicit_kept;
  kept.insert(ids.begin(), ids.end());
  return {kept, without_any(explicit_deleted, kept), group_keepers};
}

auto deletion_intent::with_mark_all(std::vector<std::string> const &ids) const -> deletion_intent {
  auto deleted = explicit_deleted;
  deleted.insert(ids.begin(), ids.end());
  return {without_any(explicit_kept, deleted), deleted, group_keepers};
}

auto deletion_intent::with_keeper(std::uint32_t group_id, std::string keeper_id) const
    -> deletion_intent {
  auto keepers = group_keepers;
  keepers[group_id] = std::move(keeper_id);
  return {explicit_kept, explicit_deleted, std::move(keepers)};
}

auto is_marked_for_deletion(deletion_intent const &plan, std::string const &id,
                            std::optional<deletion_step> step, flag_sets const &flags) -> bool {
  if (plan.is_explicitly_deleted(id)) {
    return true;
  }
  if (plan.is_explicitly_kept(id)) {
    return false;
  }
  if (!step) {
    return false;
  }
  switch (*step) {
  case deletion_step::quality:
    return flags.quality.contains(id);
  case deletion_step::junk:
    return flags.junk.contains(id);
  case deletion_step::similar: {
    auto const keeper = flags.similar_keepers.find(id);
    return keeper != flags.similar_keepers.end() && !keeper->second.empty() && keeper->second != id;
  }
  case deletion_step::video:
    return flags.video.contains(id);
  }
  return false;
}

auto similar_keeper_map(deletion_intent const &plan,
                        std::vector<similar_group_info> const &groups)
    -> std::unordered_map<std::string, std::string> {
  std::unordered_map<std::string, std::string> result;
  for (auto const &group : groups) {
    auto const keeper = plan.user_keeper_of(group.id).value_or(group.recommended_keep_id);
    for (auto const &member : group.member_ids) {
      result[member] = keeper;
    }
  }
  return result;
}

auto flag_sets_of(deletion_intent const &plan, review_flags const &flags) -> flag_sets {
  flag_sets sets;
  for (auto const &entry : flags.quality) {
    sets.quality.insert(entry.first);
  }
  for (auto const &entry : flags.junk) {
    sets.junk.insert(entry.first);
  }
  for (auto const &entry : flags.video) {
    sets.video.insert(entry.first);
  }
  sets.similar_keepers = similar_keeper_map(plan, flags.similar_groups);
  return sets;
}

auto deleted_image_ids(deletion_intent const &plan, std::vector<std::string> const &ids,
                       review_flags const &flags) -> std::vector<std::string> {
  auto const sets = flag_sets_of(plan, flags);
  constexpr auto steps = std::array{deletion_step::quality, deletion_step::junk,
                                    deletion_step::similar, deletion_step::video};
  return filtered(ids, [&](std::string const &id) {
    return std::ranges::any_of(
        steps, [&](deletion_step step) { return is_marked_for_deletion(plan, id, step, sets); });
  });
}

auto deleted_before_similar(deletion_intent const &plan, std::vector<std::string> const &ids,
                            review_flags const &flags) -> std::vector<std::string> {
  // Only quality flags count, and the group keepers do not exist yet.
  flag_sets sets;
  for (auto const &entry : flags.quality) {
    sets.quality.insert(entry.first);
  }
  return filtered(ids, [&](std::string const &id) {
    return is_marked_for_deletion(plan, id, deletion_step::quality, sets);
  });
}

auto deleted_before_junk(deletion_intent const &plan, std::vector<std::string> const &ids,
                         review_flags const &flags) -> std::vector<std::string> {
  auto sets = flag_sets_of(plan, flags);
  sets.junk.clear();
  return filtered(ids, [&](std::string const &id) {
    return is_marked_for_deletion(plan, id, deletion_step::quality, sets) ||
           is_marked_for_deletion(plan, id, deletion_step::similar, sets);
  });
}

auto deleted_before_video(deletion_intent const &plan, std::vector<std::string> const &ids,
                          review_flags const &flags) -> std::vector<std::string> {
  auto sets = flag_sets_of(plan, flags);
  sets.video.clear();
  constexpr auto steps =
      std::array{deletion_step::quality, deletion_step::junk, deletion_step::similar};
  return filtered(ids, [&](std::string const &id) {
    return std::ranges::any_of(
        steps, [&](deletion_step step) { return is_marked_for_deletion(plan, id, step, sets); });
  });
}

auto keep_ids(std::vector<std::string> const &ordered_ids, std::vector<std::string> const &deleted)
    -> std::vector<std::string> {
  std::unordered_set<std::string> const gone{deleted.begin(), deleted.end()};
  return filtered(ordered_ids, [&](std::string const &id) { return !gone.contains(id); });
}

auto deletion_reasons(std::string const &id, deletion_intent const &plan,
                      review_flags const &flags) -> std::vector<std::string> {
  std::vector<std::string> reasons;
  if (auto const quality = flags.quality.find(id); quality != flags.quality.end()) {
    for (auto const reason : quality->second.reasons) {
      reasons.emplace_back(label_of(reason));
    }
  }
  if (auto const junk = flags.junk.find(id); junk != flags.junk.end()) {
    reasons.push_back(junk->second.reason);
  }
  if (auto const video = flags.video.find(id); video != flags.video.end()) {
    auto spaced = video->second.reason;
    std::ranges::replace(spaced, '_', ' ');
    reasons.push_back(capitalized(std::move(spaced)));
  }
  auto const keepers = similar_keeper_map(plan, flags.similar_groups);
  if (auto const keeper = keepers.find(id);
      keeper != keepers.end() && !keeper->second.empty() && keeper->second != id) {
    reasons.emplace_back("Duplicate");
  }
  if (plan.is_explicitly_deleted(id)) {
    reasons.emplace_back("Marked by you");
  }
  return reasons;
}

auto marked_by_batch(std::vector<batch_info> const &batches,
                     std::unordered_map<std::string, image_info> const &images,
                     std::vector<std::string> const &deleted) -> std::vector<marked_group> {
  std::unordered_set<std::string> const gone{deleted.begin(), deleted.end()};
  std::vector<marked_group> groups;
  for (auto const &batch : batches) {
    marked_group group{batch.title, {}};
    for (auto const &id : batch.image_ids) {
      auto const image = images.find(id);
      if (gone.contains(id) && image != images.end()) {
        group.images.push_back(image->second);
      }
    }
    if (!group.images.empty()) {
      groups.push_back(std::move(group));
    }
  }
  return groups;
}

} // namespace kustavi::gui
