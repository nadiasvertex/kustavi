#include <catch2/catch_test_macros.hpp>

#include "core/backend_launch.h"

#include <fstream>

using namespace kustavi::gui;
namespace fs = std::filesystem;

namespace {

// Counts up, so a test sees the same "random" digits every time.
auto counting_random(std::uint32_t first = 0) -> random_source {
  auto next = std::make_shared<std::uint32_t>(first);
  return [next] { return (*next)++; };
}

auto scratch_directory() -> fs::path {
  auto const dir = fs::temp_directory_path() / "kustavi_backend_launch_test";
  fs::create_directories(dir);
  return dir;
}

} // namespace

TEST_CASE("an auth token is sixty-four hex digits", "[backend_launch]") {
  auto const token = generate_auth_token(counting_random());
  CHECK(token.size() == 64);
  CHECK(token.find_first_not_of("0123456789abcdef") == std::string::npos);
  CHECK(token.substr(0, 16) == "0123456789abcdef");
}

TEST_CASE("tokens from the system differ", "[backend_launch]") {
  auto const random = system_random();
  CHECK(generate_auth_token(random) != generate_auth_token(random));
}

TEST_CASE("the log file name has the date, the time and a random suffix", "[backend_launch]") {
  auto const path = default_backend_log_file("/tmp", {2026, 8, 24, 14, 30, 12}, counting_random());
  CHECK(path.parent_path() == "/tmp");
  CHECK(path.filename() == "kustavi-backend-20260824-143012-01234567.log");
}

TEST_CASE("the log file name is the same for the same inputs and differs with the suffix",
          "[backend_launch]") {
  civil_time const now{2026, 8, 24, 14, 30};
  CHECK(default_backend_log_file("/tmp", now, counting_random(1)) ==
        default_backend_log_file("/tmp", now, counting_random(1)));
  CHECK(default_backend_log_file("/tmp", now, counting_random(1)) !=
        default_backend_log_file("/tmp", now, counting_random(2)));
}

TEST_CASE("the command line has the options the back end serves with", "[backend_launch]") {
  CHECK(backend_arguments("tok", std::nullopt, std::nullopt) ==
        std::vector<std::string>{"serve", "--listen", "127.0.0.1:0", "--token", "tok"});
  CHECK(backend_arguments("tok", fs::path{"/tmp/x.log"}, 4321) ==
        std::vector<std::string>{"serve", "--listen", "127.0.0.1:0", "--token", "tok",
                                 "--log-file", "/tmp/x.log", "--parent-pid", "4321"});
}

TEST_CASE("the back end is found beside the executable", "[backend_launch]") {
  auto const dir = scratch_directory() / "beside";
  fs::create_directories(dir);
  auto const executable = dir / "kustavi";
  CHECK_FALSE(find_backend_binary(std::nullopt, executable).has_value());
#ifdef _WIN32
  auto const backend = dir / "kustavi-backend.exe";
#else
  auto const backend = dir / "kustavi-backend";
#endif
  std::ofstream{backend} << "x";
  CHECK(find_backend_binary(std::nullopt, executable) == backend);
  fs::remove_all(dir);
}

TEST_CASE("the override beats the neighbor and is the only place that is tried",
          "[backend_launch]") {
  auto const dir = scratch_directory() / "override";
  fs::create_directories(dir);
  auto const custom = dir / "custom-backend";
  std::ofstream{custom} << "x";
  CHECK(find_backend_binary(custom.string(), dir / "kustavi") == custom);
  CHECK_FALSE(find_backend_binary((dir / "missing").string(), dir / "kustavi").has_value());
  // An empty override counts as not set.
  CHECK_FALSE(find_backend_binary("", dir / "kustavi").has_value());
  fs::remove_all(dir);
}

TEST_CASE("a back end that does not start is a startup failure", "[backend_launch]") {
  backend_launch options;
  options.binary = "/no/such/kustavi-backend";
  options.token = "tok";
  auto const launched = launch_backend(std::move(options));
  REQUIRE_FALSE(launched.has_value());
  CHECK(message_of(backend_error{launched.error()}).find("Back end") == 0);
}
