#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace kustavi::gui {

// The steps of the wizard. The index of a step is what the back end saves for a resume, so the
// order must not change.
enum class wizard_step : std::uint8_t { select, quality, duplicates, junk, video, trips, copy };

[[nodiscard]] constexpr auto index_of(wizard_step step) -> int { return static_cast<int>(step); }

// The name in the step indicator.
[[nodiscard]] constexpr auto label_of(wizard_step step) -> std::string_view {
  constexpr std::array labels{"Select", "Quality", "Duplicates", "Junk", "Video", "Trips", "Copy"};
  return labels[static_cast<std::size_t>(step)];
}

} // namespace kustavi::gui
