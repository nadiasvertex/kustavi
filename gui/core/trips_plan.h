#pragma once

#include "core/domain.h"
#include "core/trip_edits.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kustavi::gui {

// What a query about trips needs to know about the photos.
struct trip_context {
  std::unordered_map<std::string, image_info> const &images;
  // The ids in scan order.
  std::vector<std::string> const &ordered_ids;
  // The photos that some step marked for deletion. They belong to no trip.
  std::unordered_set<std::string> const &deleted;
};

// The trips layout: the back end's clustering with the user's hand edits laid over it. The edits
// are per-photo moves, trips made by hand, folder names and the choice to use folders at all.
// The plan holds only the clustering and the edits. Every query works out the effective layout.
class trips_plan {
public:
  // A photo moved to this trip id belongs to no trip.
  static constexpr std::int64_t unassigned_trip = -1;
  // Hand-made trips count up from here, above any id that the back end gives.
  static constexpr std::int64_t first_user_trip_id = 1'000'000;

  // The clustering that the back end produced. A new clustering drops the edits, because they are
  // defined against the old one.
  auto set_clustering(std::vector<trip_info> trips) -> void;
  auto add_clustered_trip(trip_info trip) -> void { clustering_.push_back(std::move(trip)); }
  [[nodiscard]] auto clustering() const -> std::vector<trip_info> const & { return clustering_; }
  // Forgets every hand edit.
  auto reset_edits() -> void;

  [[nodiscard]] auto organize_into_folders() const -> bool { return organize_into_folders_; }
  auto set_organize_into_folders(bool organize) -> void { organize_into_folders_ = organize; }

  // The trips as they stand: the clustering and the hand-made trips with the photo moves applied,
  // in order of start time. A trip with no photos left is dropped. A trip that no move touched
  // keeps the back end's times, centroid and legs. A trip that lost photos keeps its legs without
  // them. A trip that gained photos becomes one leg.
  [[nodiscard]] auto effective_trips(trip_context const &context) const -> std::vector<trip_info>;
  // Trips grouped by their folder name, in order of name and then start time.
  [[nodiscard]] auto folders(trip_context const &context) const -> std::vector<trip_folder_info>;
  // The photos in no trip that are not deleted, in scan order.
  [[nodiscard]] auto unassigned_image_ids(trip_context const &context) const
      -> std::vector<std::string>;
  // How many photos the back end clustered and a step has since marked for deletion.
  [[nodiscard]] auto marked_count(trip_context const &context) const -> int;

  // Moves photos to a trip, or out of every trip when `trip_id` is empty.
  auto move_images(std::vector<std::string> const &image_ids, std::optional<std::int64_t> trip_id)
      -> void;
  // Makes a trip from `image_ids` and returns its id. The name uses the place most of the photos
  // were clustered under, then the month of the earliest photo, taken from `now` when none has a
  // date.
  auto create_trip(std::vector<std::string> const &image_ids, trip_context const &context,
                   time_ms now) -> std::int64_t;
  // An empty name is ignored.
  auto rename_folder(std::int64_t trip_id, std::string name) -> void;

  // The sub-path of every photo for the commit, from the effective trips and legs. Empty when the
  // user turned folders off. A user rename beats the back end's slug.
  [[nodiscard]] auto commit_folder_plan(trip_context const &context) const
      -> std::unordered_map<std::string, std::string>;

  // The edits as the session saves them.
  [[nodiscard]] auto edits() const -> trip_edits;
  // Lays saved edits over the current clustering. Anything that no longer fits is dropped: a
  // photo that is gone, a trip that the clustering did not make, and a rename of a trip whose
  // generated name changed.
  auto apply_edits(trip_edits const &saved,
                   std::unordered_map<std::string, image_info> const &images) -> void;

private:
  [[nodiscard]] auto all_trips() const -> std::vector<trip_info const *>;
  [[nodiscard]] auto effective_member_ids(trip_info const &base, trip_context const &context) const
      -> std::vector<std::string>;
  [[nodiscard]] auto rebuild(trip_info const &base, std::vector<std::string> const &ids,
                             trip_context const &context) const -> trip_info;
  [[nodiscard]] auto effective_folder_of(trip_info const &trip) const -> std::string;
  [[nodiscard]] auto generated_folder_of(std::int64_t trip_id) const -> std::string;
  [[nodiscard]] auto known_place_of(std::string const &image_id) const
      -> std::optional<std::string>;

  std::vector<trip_info> clustering_;
  std::vector<trip_info> user_trips_;
  // Image id to trip id, or `unassigned_trip`.
  std::map<std::string, std::int64_t> membership_;
  std::map<std::int64_t, std::string> folder_names_;
  std::int64_t next_user_trip_id_ = first_user_trip_id;
  bool organize_into_folders_ = true;
};

} // namespace kustavi::gui
