#pragma once

#include "core/backend_error.h"
#include "core/domain.h"
#include "core/wizard_step.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace kustavi::gui {

// The coarse stages in the step indicator. Passes run in any order, so the indicator shows where
// the user is in the overall flow and not which pass is running.
enum class wizard_stage : std::uint8_t { select, organize, review, copy };

inline constexpr int wizard_stage_count = 4;

[[nodiscard]] constexpr auto label_of(wizard_stage stage) -> std::string_view {
  constexpr std::array labels{"Select", "Organize", "Review", "Copy"};
  return labels[static_cast<std::size_t>(stage)];
}

// S0: the start screen.
struct wizard_start {};

// S1: a folder scan in flight.
struct wizard_scanning {
  std::string folder;
  std::uint32_t files_seen = 0;
  std::uint32_t images_found = 0;
  std::string current_path;
};

// S0-B: the folder holds a saved session, and the user chooses to resume or start fresh.
struct wizard_session_restore {
  std::string folder;
  std::uint32_t image_count = 0;
  // The step index that the back end recorded before the previous exit.
  int saved_step_index = 0;
};

// S2: the scan finished and the user checks the result.
struct wizard_confirm_folder {
  std::string folder;
  std::uint32_t image_count = 0;
  // "<id>: <reason>" for each image file that could not be read.
  std::vector<std::string> scan_errors;
};

// The scan found no images.
struct wizard_no_images {
  std::string folder;
};

// The trips pass is running.
struct wizard_trips_running {
  std::uint32_t done = 0;
  std::uint32_t total = 0;
};

// The trips review: the clustering with the user's edits laid over it.
struct wizard_trips_review {
  // The effective trips in order of start time, grouped by folder name.
  std::vector<trip_info> trips;
  std::vector<trip_folder_info> folders;
  // The photos in no trip, in scan order.
  std::vector<std::string> unassigned_ids;
  // Photos that the back end clustered and a step has since marked for deletion.
  int marked_count = 0;
};

// A call ended in an error. `step_index` is where the user was, for the step indicator.
struct wizard_failed {
  backend_error error;
  int step_index = 0;
};

using wizard_phase = std::variant<wizard_start, wizard_scanning, wizard_session_restore,
                                  wizard_confirm_folder, wizard_no_images, wizard_trips_running,
                                  wizard_trips_review, wizard_failed>;

// The step that the back end saves for a resume.
[[nodiscard]] auto step_index_of(wizard_phase const &phase) -> int;

// The indicator position over `wizard_stage`; `wizard_stage_count` means every stage is done.
[[nodiscard]] auto stage_index_of(wizard_phase const &phase) -> int;

// The same mapping from a step index.
[[nodiscard]] constexpr auto stage_index_of_step(int step_index) -> int {
  if (step_index <= 0) {
    return static_cast<int>(wizard_stage::select);
  }
  if (step_index <= 4) {
    return static_cast<int>(wizard_stage::review);
  }
  if (step_index == 5) {
    return static_cast<int>(wizard_stage::organize);
  }
  if (step_index == 6) {
    return static_cast<int>(wizard_stage::copy);
  }
  return wizard_stage_count;
}

} // namespace kustavi::gui
