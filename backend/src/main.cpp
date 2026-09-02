#include "cmd/analyze.h"
#include "cmd/init.h"
#include "cmd/serve.h"
#include "version.h"

#include <CLI/CLI.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <iostream>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct listen_address {
  std::string host;
  int port = 0;
};

/** Routes all log output to the given sinks so stdout stays reserved for the
 * KUSTAVI-READY handshake line (spec/frontend.md §3.1). OpenCV logs to
 * stdout directly (e.g. its parallel-backend registry on first use), so it
 * is silenced too. */
void route_logs(std::vector<spdlog::sink_ptr> sinks) {
  auto logger =
      std::make_shared<spdlog::logger>("kustavi", sinks.begin(), sinks.end());
  logger->set_level(spdlog::level::info);
  logger->flush_on(spdlog::level::info);
  spdlog::set_default_logger(std::move(logger));
  cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_SILENT);
}

void route_logs_to_stderr() {
  route_logs({std::make_shared<spdlog::sinks::ostream_sink_mt>(std::cerr)});
}

/** Routes all log output to [file] (appending) while mirroring it on
 * stderr, so the GUI's in-app log ring keeps working (spec/frontend.md
 * §3.1).
 * @return std::unexpected with an error message when the file cannot be
 *         opened.
 */
auto route_logs_to_file(const std::filesystem::path &file)
    -> std::expected<void, std::string> {
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  try {
    route_logs({
        std::make_shared<spdlog::sinks::basic_file_sink_mt>(file.string(),
                                                            false),
        std::make_shared<spdlog::sinks::ostream_sink_mt>(std::cerr),
    });
    return std::expected<void, std::string>{std::in_place};
  } catch (const spdlog::spdlog_ex &e) {
    return std::unexpected(e.what());
  }
}

/** Parses a "host:port" string.
 * @return The parsed address, or std::nullopt when malformed.
 */
auto parse_listen_address(std::string_view spec)
    -> std::optional<listen_address> {
  const auto colon = spec.rfind(':');
  if (colon == std::string_view::npos) {
    return std::nullopt;
  }

  const auto host = spec.substr(0, colon);
  const auto port_text = spec.substr(colon + 1);
  if (host.empty() || port_text.empty()) {
    return std::nullopt;
  }

  try {
    std::size_t consumed = 0;
    const int port = std::stoi(std::string(port_text), &consumed);
    if (consumed != port_text.size() || port < 0 || port > 65535) {
      return std::nullopt;
    }
    return listen_address{.host = std::string(host), .port = port};
  } catch (const std::exception &) {
    return std::nullopt;
  }
}
} // namespace

auto main(int argc, char **argv) -> int {
  try {
    route_logs_to_stderr();

    CLI::App app{"Kustavi Backend Server"};
    // Let the global options below be accepted after a subcommand as well
    // (`kustavi-backend serve --log-file ...`), not only before it.
    app.fallthrough();

    std::string listen;
    std::string auth_token;
    std::int64_t parent_pid = 0;
    bool verbose = false;
    bool version = false;
    std::filesystem::path folder_path;
    std::filesystem::path log_file;

    // Global flags; the GUI passes them together with the 'serve'
    // subcommand (spec/frontend.md §3.1).
    app.add_flag("-v,--verbose", verbose, "Enable verbose logging");
    app.add_flag("--version", version, "Print version and exit");
    app.add_option(
        "--log-file", log_file,
        "Also write all log output to the given file (mirrored on stderr)");

    // ==========================================
    // Define the 'init' subcommand
    // ==========================================
    auto init_cmd =
        app.add_subcommand("init", "Initialize the database and workspace");
    init_cmd->add_option("-f,--folder", folder_path,
                         "Path to the folder where the source images live.");

    // ==========================================
    // Define the 'analyze' subcommand
    // ==========================================
    auto analyze_cmd =
        app.add_subcommand("analyze", "Run analysis passes on data");
    analyze_cmd->add_option("-f,--folder", folder_path,
                            "Path to the folder where the source images live.");

    // ==========================================
    // Define the 'serve' subcommand
    // ==========================================
    auto serve_cmd = app.add_subcommand("serve", "Run gRPC server");
    serve_cmd->add_option(
        "--listen", listen,
        "Host:port to serve the gRPC API on (loopback only); port 0 "
        "= OS-assigned");
    serve_cmd->add_option("--token", auth_token,
                          "Token validated in the gRPC call metadata of every "
                          "request");
    serve_cmd->add_option("--parent-pid", parent_pid,
                          "PID of the launching GUI; the back end exits "
                          "automatically once that process is gone "
                          "(0 = disabled)");

    // parse the args and handle --help automatically
    CLI11_PARSE(app, argc, argv)

    if (!log_file.empty()) {
      const auto result = route_logs_to_file(log_file);
      if (!result.has_value()) {
        std::cerr << "Error: cannot open log file '" << log_file.string()
                  << "': " << result.error() << '\n';
        return 1;
      }
    }

    if (version) {
      std::print("kustavi server version {}\n", kustavi::version);
      return 0;
    }

    if (verbose) {
      spdlog::set_level(spdlog::level::debug);
    }

    if (serve_cmd->parsed()) {
      if (!listen.empty()) {
        const auto address = parse_listen_address(listen);
        if (!address) {
          spdlog::error("malformed --listen address '{}' (expected host:port).",
                        listen);
          return 1;
        }
        // The server must be reachable only from this machine (spec/proto.md
        // §2).
        if (address->host != "127.0.0.1" && address->host != "::1") {
          spdlog::error(
              "--listen host must be loopback (127.0.0.1 or ::1), got '{}'",
              address->host);
          return 1;
        }
        kustavi::cmd::serve(address->host, address->port, auth_token,
                            parent_pid);
        return 0;
      }
    } else if (init_cmd->parsed() || analyze_cmd->parsed()) {
      if (folder_path.empty()) {
        std::cerr << "Error: --folder option is required for 'init' command.\n";
        return 1;
      }
      if (init_cmd->parsed()) {
        kustavi::cmd::initialize(folder_path);
      } else if (analyze_cmd->parsed()) {
        kustavi::cmd::analyze(folder_path);
      }
      return 0;
    }

    std::cout << app.help();
    return 1;
  } catch (const CLI::ParseError &e) {
    std::cerr << e.what() << '\n';
    return e.get_exit_code();
  } catch (const std::exception &e) {
    std::cerr << "Fatal error: " << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "Unknown fatal error occurred.\n";
    return 1;
  }
}
