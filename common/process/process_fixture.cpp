// A program for the process tests. Its first argument picks what it does.
//
//   ready PORT          print the ready line, then wait until it is stopped
//   ready-exit CODE     print the ready line, then exit with CODE soon after
//   exit CODE           exit with CODE at once, without a ready line
//   malformed           print a ready line that has no port
//   silent              wait until it is stopped, without a ready line
//   noisy               print lines on stdout and stderr, then the ready line, then wait
//   spaced              print a ready line with trailing spaces
//   args                print each argument on a line, then the ready line, then wait

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

auto wait_forever() -> void {
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds{1});
  }
}

} // namespace

auto main(int argc, char **argv) -> int {
  std::string const mode = argc > 1 ? argv[1] : "";
  if (mode == "ready" && argc > 2) {
    std::printf("KUSTAVI-READY %s\n", argv[2]);
    std::fflush(stdout);
    wait_forever();
  }
  if (mode == "ready-exit" && argc > 2) {
    std::printf("KUSTAVI-READY 4242\n");
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    return std::atoi(argv[2]);
  }
  if (mode == "exit" && argc > 2) {
    return std::atoi(argv[2]);
  }
  if (mode == "malformed") {
    std::printf("KUSTAVI-READY nope\n");
    std::fflush(stdout);
    wait_forever();
  }
  if (mode == "silent") {
    wait_forever();
  }
  if (mode == "noisy") {
    std::printf("out one\nout two\r\n");
    std::fprintf(stderr, "err one\n");
    std::fflush(stderr);
    std::fflush(stdout);
    std::printf("KUSTAVI-READY 1234\n");
    std::fflush(stdout);
    wait_forever();
  }
  if (mode == "spaced") {
    std::printf("KUSTAVI-READY 777   \n");
    std::fflush(stdout);
    wait_forever();
  }
  if (mode == "args") {
    for (int i = 2; i < argc; ++i) {
      std::printf("arg:%s\n", argv[i]);
    }
    std::printf("KUSTAVI-READY 1\n");
    std::fflush(stdout);
    wait_forever();
  }
  return 64;
}
