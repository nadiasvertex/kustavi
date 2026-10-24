#include <catch2/catch_test_macros.hpp>

#include "core/ui_timers.h"

#include <memory>
#include <vector>

using namespace kustavi::gui;
using namespace std::chrono_literals;

namespace {

struct harness {
  harness() : timers([this] { return now; }) {}
  std::chrono::steady_clock::time_point now{};
  ui_timers timers;
};

} // namespace

TEST_CASE("a timer runs once its time comes", "[timers]") {
  harness h;
  int ran = 0;
  h.timers.after(100ms, [&] { ++ran; });
  CHECK(h.timers.run_due() == 0);
  h.now += 99ms;
  CHECK(h.timers.run_due() == 0);
  h.now += 1ms;
  CHECK(h.timers.run_due() == 1);
  CHECK(ran == 1);
  CHECK(h.timers.run_due() == 0);
}

TEST_CASE("timers run oldest first", "[timers]") {
  harness h;
  std::vector<int> order;
  h.timers.after(300ms, [&] { order.push_back(3); });
  h.timers.after(100ms, [&] { order.push_back(1); });
  h.timers.after(200ms, [&] { order.push_back(2); });
  h.now += 1s;
  CHECK(h.timers.run_due() == 3);
  CHECK(order == std::vector<int>{1, 2, 3});
}

TEST_CASE("a cancelled timer does not run", "[timers]") {
  harness h;
  int ran = 0;
  auto const id = h.timers.after(100ms, [&] { ++ran; });
  h.timers.cancel(id);
  h.timers.cancel(id);
  h.now += 1s;
  CHECK(h.timers.run_due() == 0);
  CHECK(ran == 0);
}

TEST_CASE("a task can cancel a timer that is due with it", "[timers]") {
  harness h;
  int ran = 0;
  ui_timers::timer_id second = 0;
  h.timers.after(100ms, [&] { h.timers.cancel(second); });
  second = h.timers.after(200ms, [&] { ++ran; });
  h.now += 1s;
  CHECK(h.timers.run_due() == 1);
  CHECK(ran == 0);
}

TEST_CASE("a timer that a task starts waits for its time", "[timers]") {
  harness h;
  int ran = 0;
  h.timers.after(100ms, [&] { h.timers.after(100ms, [&] { ++ran; }); });
  h.now += 100ms;
  CHECK(h.timers.run_due() == 1);
  CHECK(ran == 0);
  h.now += 100ms;
  CHECK(h.timers.run_due() == 1);
  CHECK(ran == 1);
}

TEST_CASE("next due says how long the loop may sleep", "[timers]") {
  harness h;
  CHECK_FALSE(h.timers.next_due().has_value());
  h.timers.after(500ms, [] {});
  h.timers.after(200ms, [] {});
  CHECK(h.timers.next_due() == 200ms);
  h.now += 150ms;
  CHECK(h.timers.next_due() == 50ms);
  h.now += 1s;
  CHECK(h.timers.next_due() == 0ms);
}
