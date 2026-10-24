#include <catch2/catch_test_macros.hpp>

#include "core/model_status.h"
#include "testing/fake_kustavi_client.h"

#include <memory>
#include <vector>

using namespace kustavi::gui;
using kustavi::gui::testing::fake_kustavi_client;

namespace {

auto progress(std::uint64_t done, std::uint64_t total) -> kustavi::ModelEvent {
  kustavi::ModelEvent event;
  event.mutable_progress()->set_done_bytes(done);
  event.mutable_progress()->set_total_bytes(total);
  return event;
}

auto ready() -> kustavi::ModelEvent {
  kustavi::ModelEvent event;
  event.mutable_ready()->set_model_name("qwen2.5-vl-3b");
  return event;
}

auto drain_all(ui_queue &queue) -> void {
  while (queue.pending()) {
    queue.drain();
  }
}

} // namespace

TEST_CASE("the model goes from unknown through downloading to ready", "[model_status]") {
  ui_queue queue;
  auto client = std::make_shared<fake_kustavi_client>();
  client->model_events = {progress(600, 1200), ready()};
  model_status status{queue};
  std::vector<model_prep_state> seen;
  status.on_change([&](model_prep_state const &s) { seen.push_back(s); });

  status.start(client);
  drain_all(queue);

  REQUIRE(seen.size() == 3);
  CHECK(std::holds_alternative<model_prep_unknown>(seen[0]));
  auto const *downloading = std::get_if<model_prep_downloading>(&seen[1]);
  REQUIRE(downloading != nullptr);
  CHECK(downloading->fraction() == 0.5);
  auto const *done = std::get_if<model_prep_ready>(&seen[2]);
  REQUIRE(done != nullptr);
  CHECK(done->model_name == "qwen2.5-vl-3b");
  CHECK(std::holds_alternative<model_prep_ready>(status.state()));
}

TEST_CASE("a stream that fails reports the message", "[model_status]") {
  ui_queue queue;
  auto client = std::make_shared<fake_kustavi_client>();
  client->stream_error = backend_error{status_code::internal, "download failed"};
  model_status status{queue};
  status.start(client);
  drain_all(queue);
  auto const *failed = std::get_if<model_prep_failed>(&status.state());
  REQUIRE(failed != nullptr);
  CHECK(failed->message == "download failed");
}

TEST_CASE("cancelling the download returns to unknown and drops late events", "[model_status]") {
  ui_queue queue;
  auto client = std::make_shared<fake_kustavi_client>();
  client->hold_streams = true;
  client->model_events = {progress(600, 1200)};
  model_status status{queue};
  status.start(client);
  drain_all(queue);

  status.cancel_download();
  client->release_streams();
  drain_all(queue);
  CHECK(std::holds_alternative<model_prep_unknown>(status.state()));
}

TEST_CASE("retry runs the call again", "[model_status]") {
  ui_queue queue;
  auto client = std::make_shared<fake_kustavi_client>();
  client->stream_error = backend_error{status_code::internal, "offline"};
  model_status status{queue};
  status.start(client);
  drain_all(queue);
  REQUIRE(std::holds_alternative<model_prep_failed>(status.state()));

  client->stream_error.reset();
  client->model_events = {ready()};
  status.retry_download();
  drain_all(queue);
  CHECK(std::holds_alternative<model_prep_ready>(status.state()));
  CHECK(client->ensure_model_calls == 2);
}

TEST_CASE("a null client leaves the state unknown", "[model_status]") {
  ui_queue queue;
  model_status status{queue};
  status.start(nullptr);
  CHECK(std::holds_alternative<model_prep_unknown>(status.state()));
}
