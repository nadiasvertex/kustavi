#include <catch2/catch_test_macros.hpp>

#include "core/neighbors.h"

#include <algorithm>

using namespace kustavi::gui;

namespace {

auto image(std::string id, int seconds, bool video = false, bool timed = true) -> image_info {
  image_info info;
  info.id = id;
  info.path = "/p/" + id;
  info.name = id;
  info.size_bytes = 1;
  info.width = 1;
  info.height = 1;
  if (timed) {
    info.taken = time_ms{std::chrono::seconds{1'767'225'600 + seconds}};
  }
  info.working_image_path = "/c/" + id;
  info.is_video = video;
  return info;
}

auto ids_of(std::vector<image_info> const &images) -> std::vector<std::string> {
  std::vector<std::string> ids;
  std::ranges::transform(images, std::back_inserter(ids), [](auto const &i) { return i.id; });
  return ids;
}

} // namespace

TEST_CASE("neighbors on both sides come back in time order", "[neighbors]") {
  std::vector const images{image("d", 30), image("a", 0), image("c", 20), image("b", 10)};
  auto const result = find_neighbors(images, {"b"}, {"b"});
  REQUIRE(result.contains("b"));
  CHECK(ids_of(result.at("b")) == std::vector<std::string>{"a", "c", "d"});
}

TEST_CASE("neighbors skip excluded photos, videos, untimed photos and distant shots",
          "[neighbors]") {
  std::vector const images{image("a", 0), image("flag", 10), image("also-flag", 20),
                           image("vid", 30, true), image("far", 10000),
                           image("untimed", 0, false, false)};
  auto const result = find_neighbors(images, {"flag", "also-flag"}, {"flag", "also-flag"});
  CHECK(ids_of(result.at("flag")) == std::vector<std::string>{"a"});
  CHECK(ids_of(result.at("also-flag")) == std::vector<std::string>{"a"});
}

TEST_CASE("a flagged photo with no neighbors has no entry", "[neighbors]") {
  CHECK(find_neighbors({image("only", 0)}, {"only"}, {"only"}).empty());
}

TEST_CASE("at most per_side neighbors are taken on each side", "[neighbors]") {
  std::vector<image_info> images;
  for (int i = 0; i < 9; ++i) {
    images.push_back(image(std::string(1, static_cast<char>('a' + i)), i * 5));
  }
  auto const result = find_neighbors(images, {"e"}, {"e"}, 2);
  CHECK(ids_of(result.at("e")) == std::vector<std::string>{"c", "d", "f", "g"});
  auto const one = find_neighbors(images, {"e"}, {"e"}, 1);
  CHECK(ids_of(one.at("e")) == std::vector<std::string>{"d", "f"});
}

TEST_CASE("the search passes over excluded photos and takes the next ones", "[neighbors]") {
  std::vector const images{image("a", 0), image("b", 10), image("c", 20), image("d", 30)};
  auto const result = find_neighbors(images, {"c"}, {"c", "b"}, 1);
  CHECK(ids_of(result.at("c")) == std::vector<std::string>{"a", "d"});
}

TEST_CASE("the window ends the search even if more photos follow", "[neighbors]") {
  // A gap of three minutes is larger than the two-minute window.
  std::vector const images{image("a", 0), image("b", 180), image("c", 190)};
  auto const result = find_neighbors(images, {"c"}, {"c"});
  CHECK(ids_of(result.at("c")) == std::vector<std::string>{"b"});
}
