#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace kustavi {
class ModelEvent;
}

namespace kustavi::gui {

// Where the vision model preparation stands: unknown, then downloading, then ready, or failed.
struct model_prep_unknown {};

struct model_prep_downloading {
  std::uint64_t done_bytes = 0;
  // Zero until the back end reports a total.
  std::uint64_t total_bytes = 0;
  double speed_bytes_per_second = 0;

  // 0 to 1, or nothing while the total is unknown.
  [[nodiscard]] auto fraction() const -> std::optional<double> {
    return total_bytes > 0 ? std::optional{static_cast<double>(done_bytes) /
                                           static_cast<double>(total_bytes)}
                           : std::nullopt;
  }
};

struct model_prep_ready {
  std::string model_name;
  std::uint64_t size_bytes = 0;
};

struct model_prep_failed {
  std::string message;
};

using model_prep_state =
    std::variant<model_prep_unknown, model_prep_downloading, model_prep_ready, model_prep_failed>;

// The state that `event` reports, or nothing for an event that sets neither case.
[[nodiscard]] auto model_prep_from(ModelEvent const &event) -> std::optional<model_prep_state>;

} // namespace kustavi::gui
