#include <catch2/catch_test_macros.hpp>

#include "process/ring_buffer.h"

using kustavi::process::ring_buffer;

TEST_CASE("a ring buffer accumulates lines until it is full", "[ring_buffer]") {
  ring_buffer buffer{1024};
  buffer.add_line("hello");
  buffer.add_line("world");
  CHECK(buffer.text() == "hello\nworld\n");
  CHECK(buffer.size_bytes() == 12);
}

TEST_CASE("a ring buffer drops the oldest lines to stay within its size", "[ring_buffer]") {
  ring_buffer buffer{64};
  for (int i = 0; i < 50; ++i) {
    buffer.add_line("line-" + std::to_string(i));
  }
  CHECK(buffer.size_bytes() <= 64);
  CHECK(buffer.text().ends_with("line-49\n"));
  CHECK(buffer.text().find("line-0") == std::string::npos);
  // Only whole lines remain.
  CHECK(buffer.text().starts_with("line-"));
}

TEST_CASE("a line longer than the buffer keeps its end", "[ring_buffer]") {
  ring_buffer buffer{16};
  buffer.add_line(std::string(100, 'x'));
  CHECK(buffer.size_bytes() <= 16);
  CHECK(buffer.text() == std::string(15, 'x') + "\n");
}

TEST_CASE("a long line pushes out everything before it", "[ring_buffer]") {
  ring_buffer buffer{16};
  buffer.add_line("old");
  buffer.add_line(std::string(100, 'y'));
  CHECK(buffer.text() == std::string(15, 'y') + "\n");
}
