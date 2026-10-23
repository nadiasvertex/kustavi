#include <catch2/catch_test_macros.hpp>

#include "core/backend_host.h"
#include "testing/fake_kustavi_client.h"

#include <memory>
#include <string>
#include <vector>

using namespace kustavi::gui;
using kustavi::gui::testing::fake_kustavi_client;

namespace {

// A connector that hands out a fake client. A test can make the back end "crash" by calling the
// exit callback that the host gave.
struct fake_backend {
  auto connector() -> backend_connector {
    return [this](std::function<void()> on_exit)
               -> std::expected<backend_connection, backend_startup_failed> {
      ++starts;
      exit_callback = std::move(on_exit);
      if (failure) {
        return std::unexpected(backend_startup_failed{*failure});
      }
      auto client = std::make_shared<fake_kustavi_client>();
      clients.push_back(client);
      backend_connection connection;
      connection.client = client;
      connection.log_text = [this] { return log; };
      connection.stop = [this] { ++stops; };
      return connection;
    };
  }

  int starts = 0;
  int stops = 0;
  std::string log = "backend log";
  std::optional<std::string> failure;
  std::function<void()> exit_callback;
  std::vector<std::shared_ptr<fake_kustavi_client>> clients;
};

struct harness {
  harness() : host(queue, inline_executor(), backend.connector()) {
    backend_host::handlers h;
    h.on_ready = [this](std::shared_ptr<kustavi_client> client) {
      ++ready;
      last_client = std::move(client);
    };
    h.on_crashed = [this] { ++crashed; };
    h.on_failed = [this](std::string message) { failures.push_back(std::move(message)); };
    host.set_handlers(std::move(h));
  }

  auto settle() -> void {
    while (queue.pending()) {
      queue.drain();
    }
  }

  ui_queue queue;
  fake_backend backend;
  backend_host host;
  int ready = 0;
  int crashed = 0;
  std::vector<std::string> failures;
  std::shared_ptr<kustavi_client> last_client;
};

} // namespace

TEST_CASE("starting the back end reports the client when it is ready", "[backend_host]") {
  harness h;
  CHECK(h.host.state() == backend_state::stopped);
  h.host.start();
  CHECK(h.host.state() == backend_state::starting);
  h.settle();
  CHECK(h.host.state() == backend_state::ready);
  CHECK(h.ready == 1);
  CHECK(h.last_client != nullptr);
  CHECK(h.host.log_text() == "backend log");
}

TEST_CASE("start does nothing once the back end runs", "[backend_host]") {
  harness h;
  h.host.start();
  h.host.start();
  h.settle();
  h.host.start();
  CHECK(h.backend.starts == 1);
}

TEST_CASE("a back end that cannot start reports the message", "[backend_host]") {
  harness h;
  h.backend.failure = "not found";
  h.host.start();
  h.settle();
  CHECK(h.host.state() == backend_state::failed);
  CHECK(h.failures == std::vector<std::string>{"not found"});
  CHECK(h.ready == 0);
}

TEST_CASE("an unexpected exit is reported as a crash", "[backend_host]") {
  harness h;
  h.host.start();
  h.settle();
  h.backend.exit_callback();
  h.settle();
  CHECK(h.host.state() == backend_state::crashed);
  CHECK(h.crashed == 1);
}

TEST_CASE("restart stops the old process and starts a new one", "[backend_host]") {
  harness h;
  h.host.start();
  h.settle();
  auto const first_exit = h.backend.exit_callback;
  h.host.restart();
  h.settle();
  CHECK(h.backend.stops == 1);
  CHECK(h.backend.starts == 2);
  CHECK(h.ready == 2);
  CHECK(h.last_client == h.backend.clients.back());
  // A late exit report of the old process is ignored.
  first_exit();
  h.settle();
  CHECK(h.host.state() == backend_state::ready);
  CHECK(h.crashed == 0);
}

TEST_CASE("restart after a crash starts again", "[backend_host]") {
  harness h;
  h.host.start();
  h.settle();
  h.backend.exit_callback();
  h.settle();
  h.host.restart();
  h.settle();
  CHECK(h.host.state() == backend_state::ready);
  CHECK(h.backend.starts == 2);
}

TEST_CASE("quit stops the back end and then calls done", "[backend_host]") {
  harness h;
  h.host.start();
  h.settle();
  bool done = false;
  h.host.quit([&] { done = true; });
  h.settle();
  CHECK(done);
  CHECK(h.backend.stops == 1);
  CHECK(h.host.state() == backend_state::stopped);
  // Nothing starts after quit.
  h.host.start();
  h.host.restart();
  CHECK(h.backend.starts == 1);
}

TEST_CASE("quit with no back end calls done at once", "[backend_host]") {
  harness h;
  bool done = false;
  h.host.quit([&] { done = true; });
  CHECK(done);
}

TEST_CASE("the log of a stopped process stays available", "[backend_host]") {
  harness h;
  h.host.start();
  h.settle();
  h.backend.log = "final words";
  h.host.restart();
  CHECK(h.host.log_text() == "final words");
}
