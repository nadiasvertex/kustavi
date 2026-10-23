#include "core/backend_host.h"

#include "core/grpc_kustavi_client.h"

namespace kustavi::gui {

backend_host::backend_host(ui_queue &queue, background_executor executor,
                           backend_connector connector)
    : scope_(queue), executor_(std::move(executor)), connector_(std::move(connector)) {}

auto backend_host::log_text() const -> std::string {
  return connection_ ? connection_->log_text() : last_log_;
}

auto backend_host::discard_connection() -> void {
  if (!connection_) {
    return;
  }
  last_log_ = connection_->log_text();
  // The process is stopped on a worker. The task owns the connection until it is done.
  executor_([connection = std::move(connection_)] { connection->stop(); });
}

auto backend_host::start() -> void {
  if (quitting_ || state_ == backend_state::starting || state_ == backend_state::ready) {
    return;
  }
  launch();
}

auto backend_host::restart() -> void {
  if (quitting_) {
    return;
  }
  discard_connection();
  launch();
}

auto backend_host::launch() -> void {
  state_ = backend_state::starting;
  auto const generation = scope_.next_generation();
  executor_([this, poster = scope_.get_poster(), generation, connector = connector_] {
    auto result = connector([this, poster, generation] {
      poster.post(generation, [this] {
        state_ = backend_state::crashed;
        if (handlers_.on_crashed) {
          handlers_.on_crashed();
        }
      });
    });
    poster.post(generation, [this, result = std::move(result)]() mutable {
      if (!result) {
        state_ = backend_state::failed;
        if (handlers_.on_failed) {
          handlers_.on_failed(result.error().message);
        }
        return;
      }
      connection_ = std::make_shared<backend_connection>(std::move(*result));
      state_ = backend_state::ready;
      if (handlers_.on_ready) {
        handlers_.on_ready(connection_->client);
      }
    });
  });
}

auto backend_host::quit(std::function<void()> done) -> void {
  quitting_ = true;
  scope_.next_generation();
  auto const poster = scope_.get_poster();
  auto const generation = scope_.generation();
  state_ = backend_state::stopped;
  if (!connection_) {
    // A start that is still in flight is dropped when its result arrives and is stopped with its
    // process by the destructor.
    if (done) {
      done();
    }
    return;
  }
  last_log_ = connection_->log_text();
  executor_([poster, generation, connection = std::move(connection_), done = std::move(done)]() mutable {
    connection->stop();
    poster.post(generation, std::move(done));
  });
}

auto make_process_connector(process_connector_options options) -> backend_connector {
  return [options = std::move(options)](std::function<void()> on_unexpected_exit)
             -> std::expected<backend_connection, backend_startup_failed> {
    auto const binary = find_backend_binary(options.binary_override, options.executable);
    if (!binary) {
      return std::unexpected(backend_startup_failed{"The back end program was not found."});
    }
    auto random = system_random();
    auto const token = generate_auth_token(random);
    backend_launch launch;
    launch.binary = *binary;
    launch.token = token;
    launch.parent_pid = options.parent_pid;
    launch.log_file = default_backend_log_file(options.temp_directory,
                                               to_local_civil(std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now())),
                                               random);
    launch.on_exit = [on_unexpected_exit](process::exit_info info) {
      if (!info.shutdown_requested) {
        on_unexpected_exit();
      }
    };
    auto process = launch_backend(std::move(launch));
    if (!process) {
      return std::unexpected(process.error());
    }
    std::shared_ptr<backend_process> shared{std::move(*process)};
    auto channel = connect_to_backend(shared->port());
    auto probe = probe_backend(channel, shared->token(), options.probe_timeout);
    if (!probe) {
      return std::unexpected(
          backend_startup_failed{"The back end did not answer: " + message_of(probe.error())});
    }
    auto client = std::make_shared<grpc_kustavi_client>(channel, shared->token());
    backend_connection connection;
    connection.client = client;
    connection.log_text = [shared] { return shared->log_text(); };
    connection.stop = [shared, client, timeout = options.stop_timeout] {
      shared->process().request_shutdown();
      (void)client->shutdown();
      if (!shared->process().wait_for_exit(timeout)) {
        shared->process().terminate();
        if (!shared->process().wait_for_exit(timeout)) {
          shared->process().kill();
        }
      }
    };
    return connection;
  };
}

} // namespace kustavi::gui
