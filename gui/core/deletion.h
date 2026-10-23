#pragma once

#include "core/domain.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kustavi::gui {

// What the user decided about deleting photos. Explicit choices live apart from the defaults that
// the passes set, so they survive a pass that runs later. A value: each change returns a new one.
struct deletion_intent {
  // Photos the user chose to keep.
  std::set<std::string> explicit_kept;
  // Photos the user marked for deletion.
  std::set<std::string> explicit_deleted;
  // Group id to the keeper the user chose. A group without an entry uses the recommendation.
  std::map<std::uint32_t, std::string> group_keepers;

  [[nodiscard]] auto is_explicitly_kept(std::string const &id) const -> bool {
    return explicit_kept.contains(id);
  }
  [[nodiscard]] auto is_explicitly_deleted(std::string const &id) const -> bool {
    return explicit_deleted.contains(id);
  }
  // The keeper that the user chose for a group, if any.
  [[nodiscard]] auto user_keeper_of(std::uint32_t group_id) const -> std::optional<std::string>;

  // Marks a photo that is not marked, and keeps one that is.
  [[nodiscard]] auto with_toggle(std::string const &id) const -> deletion_intent;
  [[nodiscard]] auto with_kept(std::string const &id) const -> deletion_intent;
  [[nodiscard]] auto with_marked(std::string const &id) const -> deletion_intent;
  // The last call wins for a photo named in both an earlier and a later call.
  [[nodiscard]] auto with_keep_all(std::vector<std::string> const &ids) const -> deletion_intent;
  [[nodiscard]] auto with_mark_all(std::vector<std::string> const &ids) const -> deletion_intent;
  [[nodiscard]] auto with_keeper(std::uint32_t group_id, std::string keeper_id) const
      -> deletion_intent;
};

// The pass whose default applies beneath the user's choices.
enum class deletion_step : std::uint8_t {
  // Photos flagged by blur and exposure.
  quality,
  // Photos the vision model classified as junk.
  junk,
  // Photos that are not the keeper of their group.
  similar,
  // Videos the video pass flagged.
  video,
};

// What the passes flagged, which together with a `deletion_intent` decides what is deleted.
struct review_flags {
  std::unordered_map<std::string, quality_flag_info> quality;
  std::unordered_map<std::string, junk_flag_info> junk;
  std::unordered_map<std::string, video_flag_info> video;
  std::vector<similar_group_info> similar_groups;
};

// The ids that a step has flagged, and the keeper of each group member.
struct flag_sets {
  std::unordered_set<std::string> quality;
  std::unordered_set<std::string> junk;
  std::unordered_set<std::string> video;
  std::unordered_map<std::string, std::string> similar_keepers;
};

// The order of rules is fixed:
//   1. A photo the user marked is deleted.
//   2. A photo the user kept is not.
//   3. The default of `step`: flagged by quality, flagged as junk, flagged as a video, or not the
//      keeper of its group.
//   4. Otherwise it is not deleted.
// Without a step only the first two rules apply.
[[nodiscard]] auto is_marked_for_deletion(deletion_intent const &plan, std::string const &id,
                                          std::optional<deletion_step> step,
                                          flag_sets const &flags) -> bool;

// Each group member's keeper: the user's choice, else the back end's recommendation.
[[nodiscard]] auto similar_keeper_map(deletion_intent const &plan,
                                      std::vector<similar_group_info> const &groups)
    -> std::unordered_map<std::string, std::string>;

[[nodiscard]] auto flag_sets_of(deletion_intent const &plan, review_flags const &flags)
    -> flag_sets;

// The photos deleted by any step or by the user, among `ids` and in their order.
[[nodiscard]] auto deleted_image_ids(deletion_intent const &plan,
                                     std::vector<std::string> const &ids,
                                     review_flags const &flags) -> std::vector<std::string>;
// The photos the quality step deleted, so the duplicate pass does not score them.
[[nodiscard]] auto deleted_before_similar(deletion_intent const &plan,
                                          std::vector<std::string> const &ids,
                                          review_flags const &flags) -> std::vector<std::string>;
// The photos the quality or duplicate step deleted, so the junk pass skips them.
[[nodiscard]] auto deleted_before_junk(deletion_intent const &plan,
                                       std::vector<std::string> const &ids,
                                       review_flags const &flags) -> std::vector<std::string>;
// The photos every step but the video step deleted, so the video pass skips them.
[[nodiscard]] auto deleted_before_video(deletion_intent const &plan,
                                        std::vector<std::string> const &ids,
                                        review_flags const &flags) -> std::vector<std::string>;

// The photos to copy at commit: `ordered_ids` without the deleted ones.
[[nodiscard]] auto keep_ids(std::vector<std::string> const &ordered_ids,
                            std::vector<std::string> const &deleted) -> std::vector<std::string>;

// Why a photo is deleted, one label for each reason. A photo the user marked also has
// "Marked by you".
[[nodiscard]] auto deletion_reasons(std::string const &id, deletion_intent const &plan,
                                    review_flags const &flags) -> std::vector<std::string>;

// The deleted photos grouped by batch, in batch order. A batch with none left out.
struct marked_group {
  std::string title;
  std::vector<image_info> images;
};
[[nodiscard]] auto marked_by_batch(std::vector<batch_info> const &batches,
                                   std::unordered_map<std::string, image_info> const &images,
                                   std::vector<std::string> const &deleted)
    -> std::vector<marked_group>;

} // namespace kustavi::gui
