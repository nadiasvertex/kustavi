#include "serve.h"

#include "kustavi_service.h"

#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <exception>
#include <iostream>
#include <print>
#include <string>
#include <thread>

namespace kustavi::cmd {

void serve(const std::string &host, int port, const std::string &auth_token) {
  // Note: stdout is reserved for the KUSTAVI-READY handshake line; all log
  // output is routed to stderr by main() so the GUI can capture it.
  kustavi::kustavi_service service(auth_token);

  grpc::ServerBuilder builder;
  const std::string listen_address = host + ":" + std::to_string(port);
  const auto credentials = grpc::InsecureServerCredentials();
  // Populated with the OS-assigned port after BuildAndStart succeeds.
  int bound_port = 0;
  builder.AddListeningPort(listen_address, credentials, &bound_port);
  builder.RegisterService(&service);
  // Long passes block one sync worker while they stream; keep the rest free
  // so GetInfo/Shutdown stay responsive during a pass.
  const int workers =
      std::max(2, static_cast<int>(std::thread::hardware_concurrency()));
  builder.SetSyncServerOption(
      grpc::ServerBuilder::SyncServerOption::MIN_POLLERS, workers);
  builder.SetSyncServerOption(
      grpc::ServerBuilder::SyncServerOption::MAX_POLLERS, workers);

  auto server = builder.BuildAndStart();
  if (!server) {
    throw std::runtime_error("failed to start gRPC server on " +
                             listen_address);
  }

  if (bound_port <= 0) {
    server->Shutdown();
    throw std::runtime_error("server started but reported no bound port");
  }

  // Ready handshake (spec/frontend.md §3.1): exactly one flushed line on
  // stdout carrying the OS-assigned port when 0 was requested.
  std::print("KUSTAVI-READY {}\n", bound_port);
  std::flush(std::cout);
  spdlog::info("gRPC server ready on '{}:{}'", host, bound_port);

  // The Shutdown RPC handler only flips a flag: calling server->Shutdown()
  // from the worker thread deadlocks (graceful shutdown waits for in-flight
  // calls, including the Shutdown RPC itself). Stop the server off-thread.
  while (!service.is_shutdown_requested()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  server->Shutdown();
  server->Wait();
  spdlog::info("gRPC server stopped");
}
} // namespace kustavi::cmd
