#include <catch2/catch_test_macros.hpp>

#include "core/ui_queue.h"

#include <atomic>
#include <thread>
#include <vector>

using kustavi::gui::ui_queue;

TEST_CASE("posted tasks run on drain, in order", "[ui_queue]") {
  ui_queue queue;
  std::vector<int> order;
  queue.post([&] { order.push_back(1); });
  queue.post([&] { order.push_back(2); });
  queue.post([&] { order.push_back(3); });
  CHECK(order.empty());
  CHECK(queue.pending());
  CHECK(queue.drain() == 3);
  CHECK(order == std::vector<int>{1, 2, 3});
  CHECK_FALSE(queue.pending());
  CHECK(queue.drain() == 0);
}

TEST_CASE("each post wakes the loop", "[ui_queue]") {
  int wakes = 0;
  ui_queue queue{[&] { ++wakes; }};
  queue.post([] {});
  queue.post([] {});
  CHECK(wakes == 2);
}

TEST_CASE("a task that posts a task leaves it for the next drain", "[ui_queue]") {
  ui_queue queue;
  std::vector<int> order;
  queue.post([&] {
    order.push_back(1);
    queue.post([&] { order.push_back(3); });
  });
  queue.post([&] { order.push_back(2); });
  CHECK(queue.drain() == 2);
  CHECK(order == std::vector<int>{1, 2});
  CHECK(queue.pending());
  CHECK(queue.drain() == 1);
  CHECK(order == std::vector<int>{1, 2, 3});
}

TEST_CASE("tasks from many threads all run on the draining thread", "[ui_queue]") {
  ui_queue queue;
  std::atomic<int> total{0};
  auto const ui_thread = std::this_thread::get_id();
  std::atomic<bool> wrong_thread{false};
  {
    std::vector<std::jthread> workers;
    for (int w = 0; w < 4; ++w) {
      workers.emplace_back([&] {
        for (int i = 0; i < 250; ++i) {
          queue.post([&] {
            if (std::this_thread::get_id() != ui_thread) {
              wrong_thread = true;
            }
            ++total;
          });
        }
      });
    }
  }
  while (queue.pending()) {
    queue.drain();
  }
  CHECK(total == 1000);
  CHECK_FALSE(wrong_thread);
}
