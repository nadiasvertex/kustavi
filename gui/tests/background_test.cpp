#include <catch2/catch_test_macros.hpp>

#include "core/background.h"

#include <atomic>
#include <thread>

using namespace kustavi::gui;

TEST_CASE("the inline executor runs the task at once", "[background]") {
  bool ran = false;
  inline_executor()([&] { ran = true; });
  CHECK(ran);
}

TEST_CASE("the pool runs every submitted task off the calling thread", "[background]") {
  std::atomic<int> count{0};
  std::atomic<bool> same_thread{false};
  auto const caller = std::this_thread::get_id();
  {
    worker_pool pool{3};
    auto run = pool.executor();
    for (int i = 0; i < 100; ++i) {
      run([&] {
        if (std::this_thread::get_id() == caller) {
          same_thread = true;
        }
        ++count;
      });
    }
    // Destroying the pool finishes the queued tasks.
  }
  CHECK(count == 100);
  CHECK_FALSE(same_thread);
}
