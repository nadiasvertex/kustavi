#include <catch2/catch_test_macros.hpp>

#include "testing/fake_kustavi_client.h"

#include <string>
#include <vector>

using namespace kustavi::gui;
using kustavi::gui::testing::fake_kustavi_client;

namespace {

auto scan_progress(std::uint32_t seen) -> kustavi::ScanEvent {
  kustavi::ScanEvent event;
  event.mutable_progress()->set_files_seen(seen);
  return event;
}

struct recorder {
  stream_handlers<kustavi::ScanEvent> handlers() {
    return {[this](kustavi::ScanEvent const &e) { seen.push_back(e.progress().files_seen()); },
            [this](std::optional<backend_error> e) {
              ++done_calls;
              error = std::move(e);
            }};
  }

  std::vector<std::uint32_t> seen;
  int done_calls = 0;
  std::optional<backend_error> error;
};

} // namespace

TEST_CASE("a stream delivers its events and then ends", "[fake_client]") {
  fake_kustavi_client client;
  client.scan_events = {scan_progress(1), scan_progress(2)};
  recorder rec;
  auto subscription = client.scan_folder({}, rec.handlers());
  CHECK(rec.seen == std::vector<std::uint32_t>{1, 2});
  CHECK(rec.done_calls == 1);
  CHECK_FALSE(rec.error.has_value());
}

TEST_CASE("a stream with an error ends with it after its events", "[fake_client]") {
  fake_kustavi_client client;
  client.scan_events = {scan_progress(1)};
  client.stream_error = backend_crashed{"gone"};
  recorder rec;
  auto subscription = client.scan_folder({}, rec.handlers());
  CHECK(rec.seen.size() == 1);
  REQUIRE(rec.error.has_value());
  CHECK(std::holds_alternative<backend_crashed>(*rec.error));
}

TEST_CASE("the fake records the request that it was given", "[fake_client]") {
  fake_kustavi_client client;
  kustavi::ScanFolderRequest request;
  request.set_folder("/photos");
  recorder rec;
  auto subscription = client.scan_folder(request, rec.handlers());
  REQUIRE(client.last_scan_request.has_value());
  CHECK(client.last_scan_request->folder() == "/photos");
  (void)client.inspect_session("/other");
  CHECK(client.last_inspected_folder == "/other");
}

TEST_CASE("a held stream delivers when it is released", "[fake_client]") {
  fake_kustavi_client client;
  client.hold_streams = true;
  client.scan_events = {scan_progress(7)};
  recorder rec;
  auto subscription = client.scan_folder({}, rec.handlers());
  CHECK(rec.seen.empty());
  CHECK(client.held_streams() == 1);
  client.release_streams();
  CHECK(rec.seen == std::vector<std::uint32_t>{7});
  CHECK(rec.done_calls == 1);
}

TEST_CASE("a cancelled stream delivers nothing, not even its end", "[fake_client]") {
  fake_kustavi_client client;
  client.hold_streams = true;
  client.scan_events = {scan_progress(7)};
  recorder rec;
  auto subscription = client.scan_folder({}, rec.handlers());
  subscription.cancel();
  client.release_streams();
  CHECK(rec.seen.empty());
  CHECK(rec.done_calls == 0);
}

TEST_CASE("unary calls return what was scripted, or the error", "[fake_client]") {
  fake_kustavi_client client;
  client.info.set_version("1.4.0");
  CHECK(client.get_info()->version() == "1.4.0");
  client.unary_error = backend_not_found{"missing"};
  auto const failed = client.get_info();
  REQUIRE_FALSE(failed.has_value());
  CHECK(std::holds_alternative<backend_not_found>(failed.error()));
  CHECK_FALSE(client.shutdown().has_value());
  CHECK(client.shutdown_calls == 1);
}

TEST_CASE("saved session states are kept in order", "[fake_client]") {
  fake_kustavi_client client;
  kustavi::SaveSessionStateRequest first;
  first.set_trip_edits("a");
  kustavi::SaveSessionStateRequest second;
  second.set_trip_edits("b");
  CHECK(client.save_session_state(first).has_value());
  CHECK(client.save_session_state(second).has_value());
  REQUIRE(client.saved_session_states.size() == 2);
  CHECK(client.saved_session_states[1].trip_edits() == "b");
}
