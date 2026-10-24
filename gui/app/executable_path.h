#pragma once

#include <filesystem>

namespace kustavi::gui {

// The path of the running program. Falls back to `argv0` where the system cannot say.
[[nodiscard]] auto current_executable(char const *argv0) -> std::filesystem::path;

} // namespace kustavi::gui
