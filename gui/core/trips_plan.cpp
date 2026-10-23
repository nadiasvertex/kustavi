#include "core/trips_plan.h"

#include "core/format.h"

#include <algorithm>
#include <ranges>
#include <set>

namespace kustavi::gui {

namespace {


auto contains_id(std::vector<std::string> const &ids, std::string const &id) -> bool {
  return std::ranges::find(ids, id) != ids.end();
}

} // namespace

auto trips_plan::set_clustering(std::vector<trip_info> trips) -> void {
  clustering_ = std::move(trips);
  reset_edits();
}

auto trips_plan::reset_edits() -> void {
  membership_.clear();
  user_trips_.clear();
  folder_names_.clear();
  next_user_trip_id_ = first_user_trip_id;
}

auto trips_plan::all_trips() const -> std::vector<trip_info const *> {
  std::vector<trip_info const *> trips;
  for (auto const &t : clustering_) {
    trips.push_back(&t);
  }
  for (auto const &t : user_trips_) {
    trips.push_back(&t);
  }
  return trips;
}

auto trips_plan::effective_member_ids(trip_info const &base, trip_context const &context) const
    -> std::vector<std::string> {
  std::set<std::string> ids;
  for (auto const &id : base.member_ids) {
    if (context.deleted.contains(id)) {
      continue;
    }
    auto const moved = membership_.find(id);
    if (moved == membership_.end() || moved->second == base.id) {
      ids.insert(id);
    }
  }
  for (auto const &[id, trip] : membership_) {
    if (trip == base.id && !context.deleted.contains(id)) {
      ids.insert(id);
    }
  }
  std::vector<std::string> ordered{ids.begin(), ids.end()};
  auto const taken_of = [&](std::string const &id) -> std::optional<time_ms> {
    auto const image = context.images.find(id);
    return image == context.images.end() ? std::nullopt : image->second.taken;
  };
  // Photos with a date come first in date order, and the rest follow by id. Photos with the same
  // date also go by id, so the order does not depend on the input.
  std::ranges::sort(ordered, [&](std::string const &a, std::string const &b) {
    auto const ta = taken_of(a);
    auto const tb = taken_of(b);
    if (ta.has_value() != tb.has_value()) {
      return ta.has_value();
    }
    if (ta && *ta != *tb) {
      return *ta < *tb;
    }
    return a < b;
  });
  return ordered;
}

auto trips_plan::rebuild(trip_info const &base, std::vector<std::string> const &ids,
                         trip_context const &context) const -> trip_info {
  std::unordered_set<std::string> const id_set{ids.begin(), ids.end()};
  std::unordered_set<std::string> const base_set{base.member_ids.begin(), base.member_ids.end()};
  if (id_set == base_set) {
    return base;
  }

  std::optional<time_ms> first;
  std::optional<time_ms> last;
  double latitude_sum = 0;
  double longitude_sum = 0;
  int with_gps = 0;
  for (auto const &id : ids) {
    auto const found = context.images.find(id);
    if (found == context.images.end()) {
      continue;
    }
    auto const &image = found->second;
    if (image.taken) {
      first = first ? std::min(*first, *image.taken) : *image.taken;
      last = last ? std::max(*last, *image.taken) : *image.taken;
    }
    if (image.gps) {
      latitude_sum += image.gps->latitude;
      longitude_sum += image.gps->longitude;
      ++with_gps;
    }
  }
  std::optional<geo_point> centroid;
  if (with_gps > 0) {
    centroid = geo_point{latitude_sum / with_gps, longitude_sum / with_gps};
  }

  trip_info result = base;
  result.start = first.value_or(base.start);
  result.end = last.value_or(base.end);
  result.member_ids = ids;
  if (centroid) {
    result.centroid = centroid;
  }

  bool const only_removals = std::ranges::all_of(
      ids, [&](std::string const &id) { return base_set.contains(id); });
  if (only_removals && base.legs.size() > 1) {
    // Photos only left, so the back end's legs and place names still hold. Each leg loses the
    // photos that left.
    result.legs.clear();
    for (auto const &leg : base.legs) {
      trip_leg_info kept = leg;
      kept.member_ids.clear();
      std::ranges::copy_if(leg.member_ids, std::back_inserter(kept.member_ids),
                           [&](std::string const &id) { return id_set.contains(id); });
      if (!kept.member_ids.empty()) {
        result.legs.push_back(std::move(kept));
      }
    }
    return result;
  }
  // A hand edit voids the back end's split into legs, so the trip becomes one leg.
  result.legs = {{base.place_name, base.folder_slug.empty() ? "leg-1" : base.folder_slug, ids,
                  result.centroid}};
  return result;
}

auto trips_plan::effective_trips(trip_context const &context) const -> std::vector<trip_info> {
  std::vector<trip_info> trips;
  for (auto const *base : all_trips()) {
    auto const ids = effective_member_ids(*base, context);
    if (!ids.empty()) {
      trips.push_back(rebuild(*base, ids, context));
    }
  }
  std::ranges::stable_sort(trips, {}, &trip_info::start);
  return trips;
}

auto trips_plan::effective_folder_of(trip_info const &trip) const -> std::string {
  auto const renamed = folder_names_.find(trip.id);
  return renamed != folder_names_.end() ? renamed->second : trip.folder.value_or(std::string{});
}

auto trips_plan::folders(trip_context const &context) const -> std::vector<trip_folder_info> {
  std::map<std::string, std::vector<trip_info>> by_folder;
  for (auto const &trip : effective_trips(context)) {
    by_folder[effective_folder_of(trip)].push_back(trip);
  }
  std::vector<trip_folder_info> result;
  for (auto &[name, trips] : by_folder) {
    result.push_back({name, std::move(trips)});
  }
  return result;
}

auto trips_plan::unassigned_image_ids(trip_context const &context) const
    -> std::vector<std::string> {
  std::unordered_set<std::string> assigned;
  for (auto const &trip : effective_trips(context)) {
    assigned.insert(trip.member_ids.begin(), trip.member_ids.end());
  }
  std::vector<std::string> result;
  std::ranges::copy_if(context.ordered_ids, std::back_inserter(result), [&](std::string const &id) {
    return !assigned.contains(id) && !context.deleted.contains(id);
  });
  return result;
}

auto trips_plan::marked_count(trip_context const &context) const -> int {
  if (context.deleted.empty()) {
    return 0;
  }
  std::unordered_set<std::string> clustered;
  for (auto const &trip : clustering_) {
    clustered.insert(trip.member_ids.begin(), trip.member_ids.end());
  }
  return static_cast<int>(std::ranges::count_if(
      clustered, [&](std::string const &id) { return context.deleted.contains(id); }));
}

auto trips_plan::move_images(std::vector<std::string> const &image_ids,
                             std::optional<std::int64_t> trip_id) -> void {
  for (auto const &id : image_ids) {
    membership_[id] = trip_id.value_or(unassigned_trip);
  }
}

auto trips_plan::known_place_of(std::string const &image_id) const -> std::optional<std::string> {
  for (auto const &trip : clustering_) {
    for (auto const &leg : trip.legs) {
      if (!leg.place_name.empty() && contains_id(leg.member_ids, image_id)) {
        return leg.place_name;
      }
    }
    if (!trip.place_name.empty() && contains_id(trip.member_ids, image_id)) {
      return trip.place_name;
    }
  }
  return std::nullopt;
}

auto trips_plan::create_trip(std::vector<std::string> const &image_ids,
                             trip_context const &context, time_ms now) -> std::int64_t {
  auto const id = next_user_trip_id_++;
  std::optional<time_ms> first;
  for (auto const &image_id : image_ids) {
    auto const image = context.images.find(image_id);
    if (image != context.images.end() && image->second.taken) {
      first = first ? std::min(*first, *image->second.taken) : *image->second.taken;
    }
  }
  auto const anchor = first.value_or(now);

  // The place that most of the photos were clustered under. The first place found wins a tie.
  std::vector<std::pair<std::string, int>> counts;
  for (auto const &image_id : image_ids) {
    auto const place = known_place_of(image_id);
    if (!place) {
      continue;
    }
    auto const entry = std::ranges::find(counts, *place, &std::pair<std::string, int>::first);
    if (entry == counts.end()) {
      counts.emplace_back(*place, 1);
    } else {
      ++entry->second;
    }
  }
  std::string place;
  int most = 0;
  for (auto const &[name, count] : counts) {
    if (count > most) {
      place = name;
      most = count;
    }
  }

  trip_info trip;
  trip.id = static_cast<std::uint32_t>(id);
  trip.start = anchor;
  trip.end = anchor;
  trip.place_name = place;
  if (!place.empty()) {
    trip.folder = place + " · " + format_month_year(anchor);
  } else if (first) {
    trip.folder = "Trip · " + format_month_year(anchor);
  } else {
    trip.folder = "New trip";
  }
  user_trips_.push_back(std::move(trip));
  for (auto const &image_id : image_ids) {
    membership_[image_id] = id;
  }
  return id;
}

auto trips_plan::rename_folder(std::int64_t trip_id, std::string name) -> void {
  if (!name.empty()) {
    folder_names_[trip_id] = std::move(name);
  }
}

auto trips_plan::commit_folder_plan(trip_context const &context) const
    -> std::unordered_map<std::string, std::string> {
  std::unordered_map<std::string, std::string> plan;
  if (!organize_into_folders_) {
    return plan;
  }
  for (auto const &trip : effective_trips(context)) {
    // The back end's slug has the place in it. The label's slug is for a trip that the user
    // renamed or that has no place.
    auto const renamed = folder_names_.find(trip.id);
    std::string const trip_slug = renamed != folder_names_.end() ? slugify(renamed->second)
                                  : !trip.folder_slug.empty()   ? trip.folder_slug
                                                                : slugify(effective_folder_of(trip));
    if (trip_slug.empty()) {
      continue;
    }
    if (trip.legs.size() > 1) {
      for (auto const &leg : trip.legs) {
        auto const leg_slug = leg.slug.empty() ? std::string{"leg"} : slugify(leg.slug);
        for (auto const &id : leg.member_ids) {
          plan[id] = trip_slug + "/" + leg_slug;
        }
      }
    }
    for (auto const &id : trip.member_ids) {
      plan.try_emplace(id, trip_slug);
    }
  }
  return plan;
}

auto trips_plan::generated_folder_of(std::int64_t trip_id) const -> std::string {
  for (auto const *trip : all_trips()) {
    if (trip->id == trip_id) {
      return trip->folder.value_or(std::string{});
    }
  }
  return {};
}

auto trips_plan::edits() const -> trip_edits {
  trip_edits result;
  result.membership = membership_;
  for (auto const &trip : user_trips_) {
    result.user_trips.push_back(
        {trip.id, std::chrono::duration_cast<std::chrono::milliseconds>(
                      trip.start.time_since_epoch()).count(),
         trip.folder.value_or(std::string{}), trip.place_name});
  }
  for (auto const &[trip_id, name] : folder_names_) {
    result.renames.push_back({trip_id, generated_folder_of(trip_id), name});
  }
  result.organize_into_folders = organize_into_folders_;
  return result;
}

auto trips_plan::apply_edits(trip_edits const &saved,
                             std::unordered_map<std::string, image_info> const &images) -> void {
  for (auto const &t : saved.user_trips) {
    trip_info trip;
    trip.id = static_cast<std::uint32_t>(t.id);
    trip.start = time_ms{std::chrono::milliseconds{t.start_ms}};
    trip.end = trip.start;
    trip.folder = t.folder;
    trip.place_name = t.place_name;
    user_trips_.push_back(std::move(trip));
    next_user_trip_id_ = std::max(next_user_trip_id_, t.id + 1);
  }
  std::unordered_set<std::int64_t> known;
  for (auto const *trip : all_trips()) {
    known.insert(trip->id);
  }
  for (auto const &[image_id, trip_id] : saved.membership) {
    if (images.contains(image_id) && (trip_id == unassigned_trip || known.contains(trip_id))) {
      membership_[image_id] = trip_id;
    }
  }
  for (auto const &rename : saved.renames) {
    if (known.contains(rename.trip_id) && generated_folder_of(rename.trip_id) == rename.from) {
      folder_names_[rename.trip_id] = rename.name;
    }
  }
  organize_into_folders_ = saved.organize_into_folders;
}

} // namespace kustavi::gui
