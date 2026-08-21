#include <CLI/CLI.hpp>

#include <exception>
#include <iostream>
#include <print>
#include <string_view>

constexpr std::string_view kustavi_version = "0.1";



auto main(int argc, char **argv) -> int {
  try {
    CLI::App app{"Kustavi Backend Server"};

        int port = 50051;
        bool verbose = false;
        bool version = false;

        // Bind flags directly to variables
        app.add_option("-p,--port", port, "gRPC server port")->default_val(50051);
        app.add_flag("-v,--verbose", verbose, "Enable verbose logging");
        app.add_flag("--version", version, "Print version");

        // This macro automatically parses and handles --help logic safely
        CLI11_PARSE(app, argc, argv);

        if (version) {
          std::print("kustavi server version {}\n", kustavi_version);
          return 0;
        }

        std::print("Starting server on port: '{}'\n", port);
        return 0;
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "Fatal error: " << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "Unknown fatal error occurred.\n";
    return 1;
  }
}
