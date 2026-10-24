#include <catch2/catch_test_macros.hpp>

#include "core/engine_client.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <vector>

using namespace kustavi::gui;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

// A copy of the test photos in a directory of its own, removed when the test ends.
struct photo_copy {
  fs::path root;

  photo_copy() {
    std::mt19937_64 random{std::random_device{}()};
    root = fs::temp_directory_path() / ("kustavi-client-test-" + std::to_string(random()));
    fs::create_directories(root);
    fs::copy(KUSTAVI_PHOTO_DIR, root / "photos", fs::copy_options::recursive);
  }
  ~photo_copy() {
    std::error_code error;
    fs::remove_all(root, error);
  }
  photo_copy(photo_copy const &) = delete;
  auto operator=(photo_copy const &) -> photo_copy & = delete;

  [[nodiscard]] auto folder() const -> std::string { return (root / "photos").string(); }
};

// What a stream reported. The handlers run on the stream's worker, so the fields are guarded.
struct recording {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<kustavi::ScanEvent> events;
  int done_calls = 0;
  std::optional<backend_error> error;

  auto handlers() -> stream_handlers<kustavi::ScanEvent> {
    stream_handlers<kustavi::ScanEvent> handlers;
    handlers.on_event = [this](kustavi::ScanEvent const &event) {
      std::scoped_lock lock{mutex};
      events.push_back(event);
      changed.notify_all();
    };
    handlers.on_done = [this](std::optional<backend_error> failure) {
      std::scoped_lock lock{mutex};
      ++done_calls;
      error = std::move(failure);
      changed.notify_all();
    };
    return handlers;
  }

  template <class Predicate> auto wait_for(Predicate predicate) -> bool {
    std::unique_lock lock{mutex};
    return changed.wait_for(lock, 60s, predicate);
  }
};

auto make_client() -> std::shared_ptr<engine_client> {
  return std::make_shared<engine_client>(std::make_shared<kustavi::engine>());
}

auto scan_request(photo_copy const &photos) -> kustavi::ScanFolderRequest {
  kustavi::ScanFolderRequest request;
  request.set_folder(photos.folder());
  request.set_recursive(true);
  return request;
}

} // namespace

TEST_CASE("a unary call returns the engine's reply", "[engine_client]") {
  auto const info = make_client()->get_info();
  REQUIRE(info.has_value());
  CHECK_FALSE(info->version().empty());
}

TEST_CASE("a unary call that the engine refuses returns its status", "[engine_client]") {
  auto const saved = make_client()->save_session_state({});
  REQUIRE_FALSE(saved.has_value());
  CHECK(saved.error().code == kustavi::status_code::failed_precondition);
  CHECK_FALSE(message_of(saved.error()).empty());
}

TEST_CASE("a stream delivers its events in order and then ends once", "[engine_client]") {
  photo_copy photos;
  auto const client = make_client();
  recording rec;
  auto subscription = client->scan_folder(scan_request(photos), rec.handlers());
  REQUIRE(rec.wait_for([&] { return rec.done_calls > 0; }));

  std::scoped_lock lock{rec.mutex};
  CHECK(rec.done_calls == 1);
  CHECK_FALSE(rec.error.has_value());
  REQUIRE_FALSE(rec.events.empty());
  CHECK(rec.events.back().has_complete());
  CHECK(rec.events.back().complete().images() > 0);
}

TEST_CASE("a stream that the engine refuses ends with the status and no events",
          "[engine_client]") {
  auto const client = make_client();
  std::mutex mutex;
  std::condition_variable changed;
  std::optional<backend_error> error;
  auto finished = false;
  auto events = 0;
  stream_handlers<kustavi::QualityEvent> handlers;
  handlers.on_event = [&](kustavi::QualityEvent const &) {
    std::scoped_lock lock{mutex};
    ++events;
  };
  handlers.on_done = [&](std::optional<backend_error> failure) {
    std::scoped_lock lock{mutex};
    error = std::move(failure);
    finished = true;
    changed.notify_all();
  };
  kustavi::RunQualityPassRequest request;
  request.set_blur_threshold(100.0);
  auto subscription = client->run_quality_pass(request, std::move(handlers));

  std::unique_lock lock{mutex};
  REQUIRE(changed.wait_for(lock, 30s, [&] { return finished; }));
  CHECK(events == 0);
  REQUIRE(error.has_value());
  CHECK(error->code == kustavi::status_code::failed_precondition);
}

TEST_CASE("cancelling a stream stops it without reporting that it is done", "[engine_client]") {
  photo_copy photos;
  auto const client = make_client();
  recording rec;
  auto subscription = client->scan_folder(scan_request(photos), rec.handlers());
  REQUIRE(rec.wait_for([&] { return !rec.events.empty(); }));
  subscription.cancel();

  std::scoped_lock lock{rec.mutex};
  CHECK(rec.done_calls == 0);
}

TEST_CASE("destroying a subscription cancels it and frees the engine", "[engine_client]") {
  photo_copy photos;
  auto const client = make_client();
  {
    recording rec;
    auto subscription = client->scan_folder(scan_request(photos), rec.handlers());
    REQUIRE(rec.wait_for([&] { return !rec.events.empty(); }));
  }

  // The pass that was cancelled has let go of the engine, so a new one can start.
  recording again;
  auto subscription = client->scan_folder(scan_request(photos), again.handlers());
  REQUIRE(again.wait_for([&] { return again.done_calls > 0; }));
  std::scoped_lock lock{again.mutex};
  CHECK_FALSE(again.error.has_value());
}
