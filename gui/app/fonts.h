#pragma once

#include "basalt/basalt.h"

#include <filesystem>

namespace kustavi::gui {

// The code points of the icon font that the screens use. The icon font is Material Icons, and
// its glyphs sit in the private use area, so text that holds one is drawn from that font.
namespace icon {
inline constexpr auto play = "\ue037";
inline constexpr auto pause = "\ue034";
inline constexpr auto check = "\ue5ca";
inline constexpr auto close = "\ue5cd";
inline constexpr auto back = "\ue5c4";
inline constexpr auto folder = "\ue2c7";
inline constexpr auto delete_ = "\ue872";
} // namespace icon

// The folder that holds the bundled fonts, found beside the program: in `Resources` of a macOS
// bundle, in `fonts` beside the program elsewhere, and in the source tree for a build that runs
// in place. Empty when none exists.
[[nodiscard]] auto find_font_directory(std::filesystem::path const &executable)
    -> std::filesystem::path;

// Adds the bundled UI and icon fonts, then the system fonts, which cover the scripts that the
// bundled ones do not. Returns the number of bundled faces added.
auto add_application_fonts(basalt::text::font_manager &fonts,
                           std::filesystem::path const &executable) -> std::size_t;

} // namespace kustavi::gui
