#pragma once

#include <string_view>

namespace kustavi {

/// Back end version, reported by `GetInfo` and printed by `--version`.
inline constexpr std::string_view version = "1.2.0";

} // namespace kustavi
