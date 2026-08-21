#include "analyze.h"
#include "init.h"

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>

#include <exception>
#include <iostream>
#include <print>
#include <string>
#include <string_view>

constexpr std::string_view kustavi_version = "0.1";

auto main(int argc, char **argv) -> int {
  try {
    CLI::App app{"Kustavi Backend Server"};

    int port = 50051;
    bool verbose = false;
    bool version = false;

    std::string auth_token;
    std::filesystem::path folder_path;

    // Bind flags directly to variables
    app.add_flag("-v,--verbose", verbose, "Enable verbose logging");
    app.add_flag("--version", version, "Print version and exit");

    // Require the user to provide at least one subcommand (serve, init, or
    // analyze)
    app.require_subcommand(1);

    // ==========================================
    // 2. Define the 'serve' subcommand
    // ==========================================
    auto serve_cmd =
        app.add_subcommand("serve", "Start the gRPC backend server");
    serve_cmd->add_option("-p,--port", port, "Port to host the server on")
        ->default_val(50051);
    serve_cmd->add_option(
        "-a,--auth-token", auth_token,
        "The token used to avoid privilege elevation attacks.");

    // ==========================================
    // 3. Define the 'init' subcommand
    // ==========================================
    auto init_cmd =
        app.add_subcommand("init", "Initialize the database and workspace");
    init_cmd->add_option("-f,--folder", folder_path,
                         "Path to the folder where the source images live.");

    // ==========================================
    // 4. Define the 'analyze' subcommand
    // ==========================================
    auto analyze_cmd =
        app.add_subcommand("analyze", "Run diagnostic analysis on data");
    analyze_cmd->add_option("-f,--folder", folder_path,
                            "Path to the folder where the source images live.");

    // parse the args and handle --help automatically
    CLI11_PARSE(app, argc, argv)

    if (version) {
      std::print("kustavi server version {}\n", kustavi_version);
      return 0;
    }

    if (verbose) {
      spdlog::set_level(spdlog::level::debug);
    }

    if (serve_cmd->parsed()) {

      spdlog::info("kustavi server version {} starting on port '{}'",
                   kustavi_version, port);

      spdlog::info("kustavi server shutdown");
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
    }

    return 0;
  } catch (const std::exception &e) {
    std::cerr << "Fatal error: " << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "Unknown fatal error occurred.\n";
    return 1;
  }
}
