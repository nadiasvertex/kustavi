#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kustavi::gui {

// A trip that the user made by hand.
struct user_trip_edit {
  std::int64_t id = 0;
  std::int64_t start_ms = 0;
  std::string folder;
  std::string place_name;

  [[nodiscard]] friend auto operator==(user_trip_edit const &, user_trip_edit const &)
      -> bool = default;
};

// A folder name that the user typed over a generated one. `from` is the generated name at that
// time. A later clustering that makes a different trip under the same id has another generated
// name, and the rename is then skipped.
struct folder_rename_edit {
  std::int64_t trip_id = 0;
  std::string from;
  std::string name;

  [[nodiscard]] friend auto operator==(folder_rename_edit const &, folder_rename_edit const &)
      -> bool = default;
};

// The user's hand edits to the trips layout. The session keeps them as text so that a resumed
// session can lay them over a new clustering. The back end stores the text and does not read it.
struct trip_edits {
  // Image id to trip id, or -1 for a photo pulled out of every trip.
  std::map<std::string, std::int64_t> membership;
  std::vector<user_trip_edit> user_trips;
  std::vector<folder_rename_edit> renames;
  bool organize_into_folders = true;

  [[nodiscard]] auto empty() const -> bool {
    return membership.empty() && user_trips.empty() && renames.empty() && organize_into_folders;
  }

  // Empty text when there is nothing to save.
  [[nodiscard]] auto encode() const -> std::string;
  // Nothing for empty text and for text that is not a valid encoding of a known version.
  [[nodiscard]] static auto decode(std::string_view text) -> std::optional<trip_edits>;

  [[nodiscard]] friend auto operator==(trip_edits const &, trip_edits const &) -> bool = default;
};

} // namespace kustavi::gui
