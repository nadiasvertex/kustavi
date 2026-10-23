#include <catch2/catch_test_macros.hpp>

#include "core/grpc_kustavi_client.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "proto/service.grpc.pb.h"

using namespace kustavi::gui;
using namespace std::chrono_literals;

namespace {

constexpr auto good_token = "secret";

// A back end in this process. It answers a few calls and checks the token the way the real one
// does.
class test_service final : public kustavi::Kustavi::Service {
public:
  std::atomic<int> scan_events_to_send{3};
  std::atomic<bool> stream_cancelled{false};
  std::atomic<bool> in_blocking_stream{false};

  grpc::Status GetInfo(grpc::ServerContext *context, kustavi::GetInfoRequest const *,
                       kustavi::GetInfoResponse *response) override {
    if (auto const denied = check(context); !denied.ok()) {
      return denied;
    }
    response->set_version("test-1");
    return grpc::Status::OK;
  }

  grpc::Status Shutdown(grpc::ServerContext *context, kustavi::ShutdownRequest const *,
                        kustavi::ShutdownResponse *) override {
    return check(context);
  }

  grpc::Status InspectSession(grpc::ServerContext *context,
                              kustavi::InspectSessionRequest const *request,
                              kustavi::InspectSessionResponse *) override {
    if (auto const denied = check(context); !denied.ok()) {
      return denied;
    }
    if (request->folder() == "/missing") {
      return {grpc::StatusCode::NOT_FOUND, "no such folder"};
    }
    if (request->folder() == "/boom") {
      return {grpc::StatusCode::INTERNAL, "boom"};
    }
    return grpc::Status::OK;
  }

  grpc::Status ScanFolder(grpc::ServerContext *context, kustavi::ScanFolderRequest const *,
                          grpc::ServerWriter<kustavi::ScanEvent> *writer) override {
    if (auto const denied = check(context); !denied.ok()) {
      return denied;
    }
    for (int i = 1; i <= scan_events_to_send; ++i) {
      kustavi::ScanEvent event;
      event.mutable_progress()->set_files_seen(static_cast<std::uint32_t>(i));
      writer->Write(event);
    }
    return grpc::Status::OK;
  }

  // Sends one event, then waits for the client to cancel.
  grpc::Status EnsureModel(grpc::ServerContext *context, kustavi::EnsureModelRequest const *,
                           grpc::ServerWriter<kustavi::ModelEvent> *writer) override {
    kustavi::ModelEvent event;
    event.mutable_progress()->set_done_bytes(1);
    writer->Write(event);
    in_blocking_stream = true;
    while (!context->IsCancelled()) {
      std::this_thread::sleep_for(5ms);
    }
    stream_cancelled = true;
    return {grpc::StatusCode::CANCELLED, "cancelled"};
  }

  grpc::Status RunTripsPass(grpc::ServerContext *, kustavi::RunTripsPassRequest const *,
                            grpc::ServerWriter<kustavi::TripsEvent> *) override {
    return {grpc::StatusCode::UNAVAILABLE, "going away"};
  }

private:
  static grpc::Status check(grpc::ServerContext const *context) {
    auto const &metadata = context->client_metadata();
    auto const found = metadata.find(auth_token_header);
    if (found == metadata.end() || found->second != good_token) {
      return {grpc::StatusCode::UNAUTHENTICATED, "bad token"};
    }
    return grpc::Status::OK;
  }
};

struct server_fixture {
  server_fixture() {
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    server = builder.BuildAndStart();
    REQUIRE(server != nullptr);
    REQUIRE(port > 0);
  }
  ~server_fixture() { server->Shutdown(std::chrono::system_clock::now() + 1s); }

  [[nodiscard]] auto client(std::string token = good_token) const -> grpc_kustavi_client {
    return grpc_kustavi_client{connect_to_backend(port), std::move(token)};
  }

  test_service service;
  int port = 0;
  std::unique_ptr<grpc::Server> server;
};

// Waits for events that a worker thread delivers.
struct collected {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<std::uint32_t> files;
  bool done = false;
  std::optional<backend_error> error;

  template <class Event> auto handlers() -> stream_handlers<Event> {
    return {[this](Event const &e) {
              std::scoped_lock lock{mutex};
              if constexpr (std::is_same_v<Event, kustavi::ScanEvent>) {
                files.push_back(e.progress().files_seen());
              } else {
                files.push_back(1);
              }
              changed.notify_all();
            },
            [this](std::optional<backend_error> e) {
              std::scoped_lock lock{mutex};
              done = true;
              error = std::move(e);
              changed.notify_all();
            }};
  }

  auto wait(std::function<bool()> const &predicate) -> bool {
    std::unique_lock lock{mutex};
    return changed.wait_for(lock, 10s, predicate);
  }
};

} // namespace

TEST_CASE("a unary call carries the token and returns the reply", "[grpc_client]") {
  server_fixture server;
  auto client = server.client();
  auto const info = client.get_info();
  REQUIRE(info.has_value());
  CHECK(info->version() == "test-1");
  CHECK(client.shutdown().has_value());
}

TEST_CASE("a wrong token is an rpc error with the status", "[grpc_client]") {
  server_fixture server;
  auto client = server.client("wrong");
  auto const info = client.get_info();
  REQUIRE_FALSE(info.has_value());
  REQUIRE(std::holds_alternative<backend_rpc>(info.error()));
  CHECK(std::get<backend_rpc>(info.error()).code == grpc::StatusCode::UNAUTHENTICATED);
}

TEST_CASE("statuses map to back-end errors", "[grpc_client]") {
  server_fixture server;
  auto client = server.client();
  auto const missing = client.inspect_session("/missing");
  REQUIRE_FALSE(missing.has_value());
  CHECK(std::holds_alternative<backend_not_found>(missing.error()));
  CHECK(message_of(missing.error()) == "no such folder");

  auto const boom = client.inspect_session("/boom");
  REQUIRE_FALSE(boom.has_value());
  CHECK(std::holds_alternative<backend_rpc>(boom.error()));

  CHECK(client.inspect_session("/ok").has_value());
}

TEST_CASE("a call to a back end that is gone is a crash", "[grpc_client]") {
  int port = 0;
  {
    server_fixture server;
    port = server.port;
  }
  grpc_kustavi_client client{connect_to_backend(port), good_token};
  auto const info = client.get_info();
  REQUIRE_FALSE(info.has_value());
  CHECK(std::holds_alternative<backend_crashed>(info.error()));
}

TEST_CASE("the probe reports the version or the failure", "[grpc_client]") {
  server_fixture server;
  auto const channel = connect_to_backend(server.port);
  auto const good = probe_backend(channel, good_token, 5s);
  REQUIRE(good.has_value());
  CHECK(good->version() == "test-1");
  CHECK_FALSE(probe_backend(channel, "wrong", 5s).has_value());
}

TEST_CASE("a stream delivers its events in order and then ends", "[grpc_client]") {
  server_fixture server;
  server.service.scan_events_to_send = 5;
  auto client = server.client();
  collected got;
  auto subscription = client.scan_folder({}, got.handlers<kustavi::ScanEvent>());
  REQUIRE(got.wait([&] { return got.done; }));
  CHECK(got.files == std::vector<std::uint32_t>{1, 2, 3, 4, 5});
  CHECK_FALSE(got.error.has_value());
}

TEST_CASE("a stream that fails ends with the mapped error", "[grpc_client]") {
  server_fixture server;
  auto client = server.client();
  collected got;
  auto subscription = client.run_trips_pass({}, got.handlers<kustavi::TripsEvent>());
  REQUIRE(got.wait([&] { return got.done; }));
  REQUIRE(got.error.has_value());
  CHECK(std::holds_alternative<backend_crashed>(*got.error));
}

TEST_CASE("a stream with a wrong token ends with an error and no events", "[grpc_client]") {
  server_fixture server;
  auto client = server.client("wrong");
  collected got;
  auto subscription = client.scan_folder({}, got.handlers<kustavi::ScanEvent>());
  REQUIRE(got.wait([&] { return got.done; }));
  CHECK(got.files.empty());
  REQUIRE(got.error.has_value());
  CHECK(std::get<backend_rpc>(*got.error).code == grpc::StatusCode::UNAUTHENTICATED);
}

TEST_CASE("cancelling a stream cancels the call in the back end", "[grpc_client]") {
  server_fixture server;
  auto client = server.client();
  collected got;
  auto subscription = client.ensure_model(got.handlers<kustavi::ModelEvent>());
  REQUIRE(got.wait([&] { return !got.files.empty(); }));
  subscription.cancel();
  for (int i = 0; i < 400 && !server.service.stream_cancelled; ++i) {
    std::this_thread::sleep_for(10ms);
  }
  CHECK(server.service.stream_cancelled);
  // A cancelled stream does not report its end.
  std::this_thread::sleep_for(100ms);
  std::scoped_lock lock{got.mutex};
  CHECK_FALSE(got.done);
}

TEST_CASE("destroying a subscription cancels it", "[grpc_client]") {
  server_fixture server;
  auto client = server.client();
  collected got;
  {
    auto subscription = client.ensure_model(got.handlers<kustavi::ModelEvent>());
    REQUIRE(got.wait([&] { return !got.files.empty(); }));
  }
  // The server notices the cancel within a few milliseconds.
  for (int i = 0; i < 400 && !server.service.stream_cancelled; ++i) {
    std::this_thread::sleep_for(10ms);
  }
  CHECK(server.service.stream_cancelled);
}
