#include <exception>
#include <iostream>
#include <print>

auto main(int argc, char **argv) -> int {
  try {
    std::print("kustavi server version 0.1\n");

    return 0;
  } catch (const std::exception &e) {
    std::cerr << "Fatal error: " << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "Unknown fatal error occurred.\n";
    return 1;
  }
}
