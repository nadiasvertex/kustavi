#pragma once

#include <stdexec/execution.hpp>

#if defined(__APPLE__)
#include "gcd_scheduler.h"
namespace kustavi::exec {
using default_scheduler = exec::gcd::gcd_scheduler;
}
#elif defined(_WIN32)
#include <exec/parallel_scheduler.hpp>
namespace kustavi::exec {
using default_scheduler = exec::parallel_scheduler;
}
#elif defined(__linux__)
#include <exec/linux/io_uring_context.hpp>
namespace kustavi::exec {
using default_scheduler = exec::io_uring_scheduler;
}
#else
#include <exec/static_thread_pool.hpp>
namespace kustavi::exec {
using default_scheduler = exec::static_thread_pool::scheduler_type;
}
#endif

namespace kustavi::exec {
inline default_scheduler make_scheduler() {
#if defined(__APPLE__)
  return exec::gcd::gcd_scheduler();
#elif defined(_WIN32)
  return exec::parallel_scheduler();
#elif defined(__linux__)
  static exec::io_uring_context context;
  static std::thread context_driver([]() { context.run(); });
  static bool detached = ([]() {
    context_driver.detach();
    return true;
  })();
  return context.get_scheduler();
#else
  static exec::static_thread_pool pool{std::thread::hardware_concurrency()};
  return pool.get_scheduler();
#endif
}
} // namespace kustavi::exec
