#include "process/ring_buffer.h"

namespace kustavi::process {

auto ring_buffer::add_line(std::string_view line) -> void {
  text_.append(line);
  text_.push_back('\n');
  // Whole lines go first, so the log never starts in the middle of a line unless one line alone is
  // longer than the buffer.
  while (text_.size() > max_bytes_) {
    auto const end = text_.find('\n');
    if (end == std::string::npos || end + 1 >= text_.size()) {
      text_.erase(0, text_.size() - max_bytes_);
      return;
    }
    text_.erase(0, end + 1);
  }
}

} // namespace kustavi::process
