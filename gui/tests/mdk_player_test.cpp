// Plays the test videos with the real libmdk player. It is built only with KUSTAVI_MDK_DIR.
#include <catch2/catch_test_macros.hpp>

#include "app/video_player.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace kustavi::gui;

namespace {

auto wait_for_frame(video_player &player, std::chrono::seconds limit) -> std::optional<video_frame> {
  auto const end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) {
    if (auto frame = player.take_frame()) {
      return frame;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  }
  return std::nullopt;
}

} // namespace

TEST_CASE("the mdk player makes frames no larger than the limit", "[video][mdk]") {
  auto const factory = default_video_player_factory();
  REQUIRE(factory);
  std::atomic<int> wakes{0};
  auto player = factory(KUSTAVI_VIDEO_DIR "/normal.mp4", [&] { ++wakes; }, 640);
  REQUIRE(player != nullptr);
  CHECK_FALSE(player->playing());
  // The first frame is up before the playback starts.
  auto first = wait_for_frame(*player, std::chrono::seconds{15});
  REQUIRE(first.has_value());
  CHECK(std::max(first->width, first->height) <= 640);
  CHECK(first->width > 0);
  CHECK(first->rgba.size() == std::size_t{first->width} * first->height * 4);
  CHECK(wakes > 0);

  player->play();
  CHECK(player->playing());
  auto next = wait_for_frame(*player, std::chrono::seconds{15});
  CHECK(next.has_value());
  player->pause();
  CHECK_FALSE(player->playing());
  CHECK_FALSE(player->failed());
}

TEST_CASE("the mdk player reports a file that cannot be played", "[video][mdk]") {
  auto const factory = default_video_player_factory();
  REQUIRE(factory);
  auto player = factory("/does/not/exist.mp4", [] {}, 640);
  REQUIRE(player != nullptr);
  auto const end = std::chrono::steady_clock::now() + std::chrono::seconds{15};
  while (!player->failed() && std::chrono::steady_clock::now() < end) {
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  }
  CHECK(player->failed());
}
