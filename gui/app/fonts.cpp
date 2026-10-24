#include "app/fonts.h"

#include <array>
#include <system_error>

namespace kustavi::gui {

namespace {

auto holds_fonts(std::filesystem::path const &directory) -> bool {
  std::error_code error;
  return std::filesystem::exists(directory / "NotoSans-Regular.ttf", error);
}

} // namespace

auto find_font_directory(std::filesystem::path const &executable) -> std::filesystem::path {
  auto const directory = executable.parent_path();
  auto const candidates = std::array<std::filesystem::path, 3>{
      directory.parent_path() / "Resources" / "fonts",
      directory / "fonts",
#ifdef KUSTAVI_FONT_DIR
      KUSTAVI_FONT_DIR,
#else
      std::filesystem::path{},
#endif
  };
  for (auto const &candidate : candidates) {
    if (!candidate.empty() && holds_fonts(candidate)) {
      return candidate;
    }
  }
  return {};
}

auto add_application_fonts(basalt::text::font_manager &fonts,
                           std::filesystem::path const &executable) -> std::size_t {
  std::size_t added = 0;
  if (auto const directory = find_font_directory(executable); !directory.empty()) {
    added = fonts.add_directory(directory, false);
  }
  fonts.add_system_fonts();
  return added;
}

} // namespace kustavi::gui
