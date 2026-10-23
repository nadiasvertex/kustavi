#include <catch2/catch_test_macros.hpp>

#include "process/supervised_process.h"

#include <atomic>
#include <csignal>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/types.h>
#endif

using namespace std::chrono_literals;
using namespace kustavi::process;

namespace {

auto options_for(std::vector<std::string> args) -> launch_options {
  launch_options options;
  options.spawn.binary = PROCESS_FIXTURE;
  options.spawn.args = std::move(args);
  options.ready_timeout = 10s;
  return options;
}

} // namespace

TEST_CASE("a process that prints its ready line is launched with that port", "[process]") {
  auto launched = supervised_process::launch(options_for({"ready", "4242"}));
  REQUIRE(launched.has_value());
  CHECK((*launched)->port() == 4242);
  CHECK((*launched)->pid() > 0);
  CHECK_FALSE((*launched)->has_exited());
}

TEST_CASE("trailing spaces after the port are allowed", "[process]") {
  auto launched = supervised_process::launch(options_for({"spaced"}));
  REQUIRE(launched.has_value());
  CHECK((*launched)->port() == 777);
}

TEST_CASE("the log keeps both streams and leaves out the ready line", "[process]") {
  std::mutex mutex;
  std::vector<std::string> seen;
  auto options = options_for({"noisy"});
  options.on_output = [&](std::string_view line) {
    std::scoped_lock lock{mutex};
    seen.emplace_back(line);
  };
  auto launched = supervised_process::launch(std::move(options));
  REQUIRE(launched.has_value());
  CHECK((*launched)->port() == 1234);
  auto const log = (*launched)->log_text();
  CHECK(log.find("out one\n") != std::string::npos);
  // A carriage return before the newline is not part of the line.
  CHECK(log.find("out two\n") != std::string::npos);
  CHECK(log.find("\r") == std::string::npos);
  CHECK(log.find("KUSTAVI-READY") == std::string::npos);
  // The error stream arrives on its own thread, so give it a moment.
  for (int i = 0; i < 100 && (*launched)->log_text().find("err one") == std::string::npos; ++i) {
    std::this_thread::sleep_for(20ms);
  }
  CHECK((*launched)->log_text().find("err one\n") != std::string::npos);
  std::scoped_lock lock{mutex};
  CHECK(seen.size() >= 2);
}

TEST_CASE("arguments reach the process unchanged", "[process]") {
  auto launched = supervised_process::launch(
      options_for({"args", "plain", "with space", "quote\"inside", "back\\slash\\", ""}));
  REQUIRE(launched.has_value());
  auto const log = (*launched)->log_text();
  CHECK(log.find("arg:plain\n") != std::string::npos);
  CHECK(log.find("arg:with space\n") != std::string::npos);
  CHECK(log.find("arg:quote\"inside\n") != std::string::npos);
  CHECK(log.find("arg:back\\slash\\\n") != std::string::npos);
  CHECK(log.find("arg:\n") != std::string::npos);
}

TEST_CASE("a ready line that cannot be read fails the launch", "[process]") {
  auto launched = supervised_process::launch(options_for({"malformed"}));
  REQUIRE_FALSE(launched.has_value());
  CHECK(launched.error().find("ready line") != std::string::npos);
}

TEST_CASE("a process that exits first fails the launch with its code", "[process]") {
  auto launched = supervised_process::launch(options_for({"exit", "3"}));
  REQUIRE_FALSE(launched.has_value());
  CHECK(launched.error().find("exited (code 3)") != std::string::npos);
}

TEST_CASE("a process that stays quiet fails the launch after the timeout", "[process]") {
  auto options = options_for({"silent"});
  options.ready_timeout = 300ms;
  auto const start = std::chrono::steady_clock::now();
  auto launched = supervised_process::launch(std::move(options));
  REQUIRE_FALSE(launched.has_value());
  CHECK(launched.error().find("not signal readiness in time") != std::string::npos);
  CHECK(std::chrono::steady_clock::now() - start < 5s);
}

TEST_CASE("a program that is not there fails the launch", "[process]") {
  launch_options options;
  options.spawn.binary = "/no/such/program";
  auto const launched = supervised_process::launch(std::move(options));
  REQUIRE_FALSE(launched.has_value());
  CHECK(launched.error().find("could not start") != std::string::npos);
}

TEST_CASE("an exit after the process was ready is reported with its code", "[process]") {
  std::promise<exit_info> reported;
  auto options = options_for({"ready-exit", "5"});
  options.on_exit = [&](exit_info info) { reported.set_value(info); };
  auto launched = supervised_process::launch(std::move(options));
  REQUIRE(launched.has_value());
  auto future = reported.get_future();
  REQUIRE(future.wait_for(10s) == std::future_status::ready);
  auto const info = future.get();
  CHECK(info.code == 5);
  CHECK_FALSE(info.shutdown_requested);
  CHECK((*launched)->has_exited());
}

TEST_CASE("an exit that was asked for says so", "[process]") {
  std::promise<exit_info> reported;
  auto options = options_for({"ready", "9"});
  options.on_exit = [&](exit_info info) { reported.set_value(info); };
  auto launched = supervised_process::launch(std::move(options));
  REQUIRE(launched.has_value());
  (*launched)->request_shutdown();
  CHECK((*launched)->shutdown_requested());
  (*launched)->terminate();
  auto future = reported.get_future();
  REQUIRE(future.wait_for(10s) == std::future_status::ready);
  CHECK(future.get().shutdown_requested);
}

TEST_CASE("waiting for an exit gives up after the timeout", "[process]") {
  auto launched = supervised_process::launch(options_for({"ready", "9"}));
  REQUIRE(launched.has_value());
  CHECK_FALSE((*launched)->wait_for_exit(100ms).has_value());
  (*launched)->kill();
  auto const code = (*launched)->wait_for_exit(10s);
  REQUIRE(code.has_value());
  CHECK(*code != 0);
}

TEST_CASE("destroying the supervisor ends the process", "[process]") {
  auto launched = supervised_process::launch(options_for({"ready", "9"}));
  REQUIRE(launched.has_value());
  [[maybe_unused]] auto const pid = (*launched)->pid();
  launched->reset();
#ifndef _WIN32
  // The process was ended and collected, so no process has that id any more.
  CHECK(::kill(static_cast<pid_t>(pid), 0) != 0);
#endif
}
