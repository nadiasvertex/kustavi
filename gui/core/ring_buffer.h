#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace kustavi::gui {

// The most recent lines of the back end's output. Old lines go first when the buffer is full.
// Not thread-safe; the owner locks it.
class ring_buffer {
public:
  explicit ring_buffer(std::size_t max_bytes) : max_bytes_(max_bytes) {}

  [[nodiscard]] auto size_bytes() const -> std::size_t { return text_.size(); }
  [[nodiscard]] auto max_bytes() const -> std::size_t { return max_bytes_; }

  // Appends `line` and a newline. A line longer than the whole buffer keeps its end.
  auto add_line(std::string_view line) -> void;

  [[nodiscard]] auto text() const -> std::string const & { return text_; }

private:
  std::size_t max_bytes_;
  std::string text_;
};

} // namespace kustavi::gui
