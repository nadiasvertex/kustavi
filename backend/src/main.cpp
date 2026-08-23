#include "analyze.h"
#include "init.h"
#include "serve.h"
#include "version.h"

#include <CLI/CLI.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <print>
#include <string>
#include <string_view>

namespace {

struct listen_address {
  std::string host;
  int port = 0;
};

/** Routes all log output to stderr so stdout stays reserved for the
 * KUSTAVI-READY handshake line (spec/frontend.md §3.1). OpenCV logs to
 * stdout directly (e.g. its parallel-backend registry on first use), so it
 * is silenced too. */
void route_logs_to_stderr() {
  auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(std::cerr);
  auto logger = std::make_shared<spdlog::logger>("kustavi", sink);
  logger->set_level(spdlog::level::info);
  logger->flush_on(spdlog::level::info);
  spdlog::set_default_logger(std::move(logger));
  cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_SILENT);
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

    std::string listen;
    std::string auth_token;
    bool verbose = false;
    bool version = false;
    std::filesystem::path folder_path;

    // --listen / --token are top-level so the GUI can launch
    // `kustavi-backend --listen 127.0.0.1:0 --token <t>` without a
    // subcommand (spec/frontend.md §3.1).
    app.add_flag("-v,--verbose", verbose, "Enable verbose logging");
    app.add_flag("--version", version, "Print version and exit");

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

    // parse the args and handle --help automatically
    CLI11_PARSE(app, argc, argv)

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
        kustavi::cmd::serve(address->host, address->port, auth_token);
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
