#include <grpcpp/grpcpp.h>

#include <proto/service.grpc.pb.h>

#include <array>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <print>
#include <random>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;
namespace k = kustavi;

namespace {

constexpr int k_fail = 1;

[[noreturn]] void fail(const std::string &message) {
  std::println(stderr, "FAIL: {}", message);
  std::exit(k_fail);
}

struct options {
  std::string target; // empty = spawn a local server; otherwise connect
  std::string token;
  std::string server; // path to the server binary (auto-detected if empty)
  std::string folder;
  std::string destination;
  bool no_auth = false;
  bool shutdown = false;
  bool cancel_check = false;
  bool concurrency_check = false;
  bool junk_check = false; // opt-in: downloads the ~3.7 GB vision model
};

auto parse_args(int argc, char **argv) -> options {
  std::vector<std::string> args;
  if (argc > 1) {
    args.reserve(static_cast<std::size_t>(argc - 1));
  }
  for (int i = 1; i < argc; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) argv
    args.emplace_back(argv[i]);
  }

  options opts;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const auto &arg = args[i];
    if (arg == "--target" && i + 1 < args.size()) {
      opts.target = args[++i];
    } else if (arg == "--server" && i + 1 < args.size()) {
      opts.server = args[++i];
    } else if (arg == "--token" && i + 1 < args.size()) {
      opts.token = args[++i];
    } else if (arg == "--folder" && i + 1 < args.size()) {
      opts.folder = args[++i];
    } else if (arg == "--destination" && i + 1 < args.size()) {
      opts.destination = args[++i];
    } else if (arg == "--no-auth") {
      opts.no_auth = true;
    } else if (arg == "--shutdown") {
      opts.shutdown = true;
    } else if (arg == "--cancel-check") {
      opts.cancel_check = true;
    } else if (arg == "--concurrency-check") {
      opts.concurrency_check = true;
    } else if (arg == "--junk-check") {
      opts.junk_check = true;
    } else {
      fail("unknown argument: " + arg);
    }
  }
  return opts;
}

auto make_channel(const options &opts) -> std::shared_ptr<grpc::Channel> {
  return grpc::CreateChannel(opts.target, grpc::InsecureChannelCredentials());
}

void add_auth_metadata(grpc::ClientContext &context, const options &opts) {
  if (!opts.no_auth) {
    context.AddMetadata("x-kustavi-auth-token", opts.token);
  }
}

// ---------------------------------------------------------------------------
// Local server process management
// ---------------------------------------------------------------------------

constexpr std::string_view k_ready_line_prefix = "KUSTAVI-READY ";

/** A spawned server: pid plus the read end of the pipe carrying the server's
 * stdout (reserved for the KUSTAVI-READY handshake). */
struct server_process {
  pid_t pid = -1;
  int stdout_fd = -1;
};

std::atomic<pid_t> g_server_pid{-1};

auto close_fd(int &fd) -> void {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

/** Full path of this executable, with symlinks resolved. */
auto own_executable_path() -> fs::path {
  std::array<char, 4096> buffer{};
  fs::path path;
#ifdef __APPLE__
  auto size = static_cast<uint32_t>(buffer.size());
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    fail("could not resolve own executable path");
  }
#else
  const auto len =
      ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (len < 0) {
    fail("could not resolve own executable path");
  }
  buffer[len] = '\0';
#endif
  path = fs::path{buffer.data()};
  std::error_code ec;
  if (auto canonical = fs::canonical(path, ec); !ec) {
    return canonical;
  }
  return path;
}

/** Locates the server binary. Order: an explicit --server path, the bazel
 * runfiles manifest, common runfiles layouts, a sibling of this executable,
 * and ./bazel-bin/backend/server. Fails the process if none is found. */
auto find_server_binary(const options &opts) -> fs::path {
  if (!opts.server.empty()) {
    fs::path path{opts.server};
    if (fs::is_regular_file(path)) {
      return path;
    }
    fail("server binary not found: " + opts.server);
  }

  const auto exe = own_executable_path();
  const auto runfiles_root =
      exe.parent_path() / (exe.filename().string() + ".runfiles");

  // The runfiles MANIFEST maps runfiles paths to real output paths regardless
  // of the repo-prefix scheme in use.
  std::ifstream manifest(runfiles_root / "MANIFEST");
  if (manifest) {
    std::string line;
    while (std::getline(manifest, line)) {
      const auto sep = line.find(' ');
      if (sep == std::string::npos) {
        continue;
      }
      const auto key = line.substr(0, sep);
      if (key == "backend/server" || key.ends_with("/backend/server")) {
        fs::path real{line.substr(sep + 1)};
        if (fs::is_regular_file(real)) {
          return real;
        }
      }
    }
  }

  for (const auto &prefix :
       {std::string{}, std::string{"_main/"}, std::string{"kustavi/"}}) {
    auto candidate = runfiles_root / (prefix + "backend/server");
    if (fs::is_regular_file(candidate)) {
      return candidate;
    }
  }

  auto sibling = exe.parent_path() / "server";
  if (fs::is_regular_file(sibling)) {
    return sibling;
  }

  auto bazel_bin = fs::path{"bazel-bin"} / "backend" / "server";
  if (fs::is_regular_file(bazel_bin)) {
    return bazel_bin;
  }

  fail("could not locate the server binary; pass --server <path> or "
       "--target <host:port>");
}

auto generate_token() -> std::string {
  std::mt19937_64 rng(std::random_device{}());
  constexpr std::string_view digits = "0123456789abcdef";
  std::string token;
  token.reserve(32);
  for (int i = 0; i < 32; ++i) {
    token.push_back(digits[rng() & 15u]);
  }
  return token;
}

/** Spawns the server bound to an ephemeral loopback port, authenticating
 * with `token`. The server's stdout is captured on a pipe for the
 * KUSTAVI-READY handshake; stderr is inherited so server logs stay visible.
 * @return std::nullopt on spawn failure. */
auto spawn_server(const fs::path &binary, const std::string &token)
    -> std::optional<server_process> {
  std::array<int, 2> pipe_fds{-1, -1};
  if (::pipe(pipe_fds.data()) != 0) {
    std::println(stderr, "pipe failed: {}", std::strerror(errno));
    return std::nullopt;
  }

  posix_spawn_file_actions_t actions{};
  if (posix_spawn_file_actions_init(&actions) != 0 ||
      posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], /*stdout=*/1) !=
          0 ||
      posix_spawn_file_actions_addopen(&actions, /*stdin=*/0, "/dev/null",
                                       O_RDONLY, 0) != 0) {
    close_fd(pipe_fds[0]);
    close_fd(pipe_fds[1]);
    std::println(stderr, "posix_spawn_file_actions setup failed");
    return std::nullopt;
  }

  // The command vectors must outlive the spawn call; the char* array points
  // into them.
  std::vector<std::string> command = {binary.c_str(), "serve",   "--listen",
                                      "127.0.0.1:0",  "--token", token};
  std::vector<char *> argv;
  argv.reserve(command.size() + 1);
  for (auto &arg : command) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);
  pid_t pid = -1;
  const auto rc = ::posix_spawn(&pid, binary.c_str(), &actions, nullptr,
                                argv.data(), nullptr);
  posix_spawn_file_actions_destroy(&actions);
  if (rc != 0) {
    close_fd(pipe_fds[0]);
    close_fd(pipe_fds[1]);
    std::println(stderr, "posix_spawn failed: {}", std::strerror(rc));
    return std::nullopt;
  }

  close_fd(pipe_fds[1]);
  return server_process{.pid = pid, .stdout_fd = pipe_fds[0]};
}

/** Reads one line from `stream` (a stdio wrapper on the server's stdout
 * pipe).
 * @return false on EOF or read error. */
auto read_line(FILE *stream, std::string &line) -> bool {
  line.clear();
  int c = ::fgetc(stream);
  while (c != EOF && c != '\n') {
    line.push_back(static_cast<char>(c));
    c = ::fgetc(stream);
  }
  if (c == EOF) {
    return (ferror(stream) == 0) && !line.empty();
  }
  return true;
}

/** Reads the server's captured stdout until the KUSTAVI-READY line arrives,
 * forwarding any other line to stderr.
 * @return the bound port, or std::nullopt on EOF or timeout. */
auto wait_for_ready(server_process &server, std::chrono::seconds timeout)
    -> std::optional<int> {
  auto *stream = ::fdopen(server.stdout_fd, "r");
  if (stream == nullptr) {
    std::println(stderr, "fdopen failed: {}", std::strerror(errno));
    close_fd(server.stdout_fd);
    return std::nullopt;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::string line;
  while (true) {
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::seconds{0}) {
      break;
    }
    pollfd pfd;
    pfd.fd = server.stdout_fd;
    pfd.events = static_cast<std::int16_t>(POLLIN);
    pfd.revents = 0;
    const auto rc = ::poll(
        &pfd, 1,
        static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(remaining)
                .count()));
    if (rc <= 0) {
      break; // timeout or poll error
    }
    if (!read_line(stream, line)) {
      break; // EOF: the server exited before becoming ready
    }
    if (line.starts_with(k_ready_line_prefix)) {
      const auto text = line.substr(k_ready_line_prefix.size());
      int port = 0;
      // from_chars takes raw [first, last) pointers; no span overload exists.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      const auto *end = text.data() + text.size();
      const auto parsed = std::from_chars(text.data(), end, port);
      if (parsed.ec != std::errc{} || parsed.ptr != end || port <= 0) {
        break;
      }
      ::fclose(stream);
      server.stdout_fd = -1;
      return port;
    }
    std::println(stderr, "[server] {}", line);
  }
  ::fclose(stream);
  server.stdout_fd = -1;
  return std::nullopt;
}

/** Polls for `pid` to exit for up to `grace`; if the process is still alive
 * on the first poll, sends it `signum`. Returns true once the child has been
 * reaped. */
auto reap_within(pid_t pid, std::chrono::milliseconds grace, int signum)
    -> bool {
  const auto deadline = std::chrono::steady_clock::now() + grace;
  bool signalled = false;
  while (std::chrono::steady_clock::now() < deadline) {
    int status = 0;
    const auto rc = ::waitpid(pid, &status, WNOHANG);
    if (rc > 0) {
      return true;
    }
    if (rc < 0) {
      return errno == ECHILD;
    }
    if (!signalled) {
      ::kill(pid, signum);
      signalled = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
  }
  return false;
}

/** Kills and reaps a leftover server (atexit / signal cleanup path). */
auto reap_server_on_exit() -> void {
  const auto pid = g_server_pid.exchange(-1);
  if (pid <= 0) {
    return;
  }
  reap_within(pid, std::chrono::seconds{5}, SIGTERM);
  reap_within(pid, std::chrono::seconds{2}, SIGKILL);
  int status = 0;
  ::waitpid(pid, &status, 0);
}

auto handle_termination_signal(int) -> void {
  const auto pid = g_server_pid.exchange(-1);
  if (pid > 0) {
    ::kill(pid, SIGKILL);
  }
  ::_exit(130);
}

/** Gracefully stops a spawned server: best-effort Shutdown RPC, then
 * SIGTERM, then SIGKILL, and reaps the child. */
auto teardown_server(server_process &server, const options &opts) -> void {
  if (server.pid <= 0) {
    return;
  }
  g_server_pid = -1; // this path reaps the child; skip the atexit helper
  if (!opts.token.empty() && !opts.no_auth) {
    auto stub = k::Kustavi::NewStub(make_channel(opts));
    k::ShutdownRequest request;
    k::ShutdownResponse response;
    grpc::ClientContext context;
    add_auth_metadata(context, opts);
    (void)stub->Shutdown(&context, request, &response);
  }
  reap_within(server.pid, std::chrono::seconds{10}, SIGTERM);
  reap_within(server.pid, std::chrono::seconds{5}, SIGKILL);
  int status = 0;
  ::waitpid(server.pid, &status, 0);
  server.pid = -1;
  close_fd(server.stdout_fd);
}

void check_get_info(const options &opts, const bool expect_auth_failure) {
  auto stub = k::Kustavi::NewStub(make_channel(opts));
  k::GetInfoRequest request;
  k::GetInfoResponse response;
  grpc::ClientContext context;
  if (!expect_auth_failure) {
    add_auth_metadata(context, opts);
  }
  const auto status = stub->GetInfo(&context, request, &response);
  if (expect_auth_failure) {
    if (status.error_code() != grpc::StatusCode::UNAUTHENTICATED) {
      fail("GetInfo without token: expected UNAUTHENTICATED, got " +
           status.error_message());
    }
    std::println("ok: GetInfo without token -> UNAUTHENTICATED");
    return;
  }
  if (!status.ok()) {
    fail("GetInfo: " + status.error_message());
  }
  std::println("ok: GetInfo version={} formats={} model={}", response.version(),
               response.supported_formats_size(), response.model_name());
}

std::vector<std::string> g_image_ids;

void run_scan(const options &opts) {
  auto stub = k::Kustavi::NewStub(make_channel(opts));
  k::ScanFolderRequest request;
  request.set_folder(opts.folder);
  request.set_recursive(true);
  grpc::ClientContext context;
  add_auth_metadata(context, opts);
  auto reader = stub->ScanFolder(&context, request);
  k::ScanEvent event;
  std::size_t images = 0;
  std::size_t progress_events = 0;
  uint32_t complete_images = 0;
  while (reader->Read(&event)) {
    if (event.has_image()) {
      images++;
      g_image_ids.push_back(event.image().id());
    } else if (event.has_progress()) {
      progress_events++;
    } else if (event.has_complete()) {
      complete_images = event.complete().images();
    }
  }
  const auto status = reader->Finish();
  if (!status.ok()) {
    fail("ScanFolder: " + status.error_message());
  }
  if (images == 0 || images != complete_images) {
    fail("ScanFolder: streamed " + std::to_string(images) +
         " images but complete reported " + std::to_string(complete_images));
  }
  std::println("ok: ScanFolder images={} progress_events={}", images,
               progress_events);
}

void run_quality(const options &opts) {
  auto stub = k::Kustavi::NewStub(make_channel(opts));
  k::RunQualityPassRequest request;
  // The server requires a positive blur threshold; mirror the GUI defaults.
  request.set_blur_threshold(100.0);
  request.set_underexposed_threshold(0.3);
  request.set_overexposed_threshold(0.3);
  grpc::ClientContext context;
  add_auth_metadata(context, opts);
  auto reader = stub->RunQualityPass(&context, request);
  k::QualityEvent event;
  std::size_t flags = 0;
  uint32_t total = 0;
  while (reader->Read(&event)) {
    if (event.has_flag()) {
      flags++;
    } else if (event.has_complete()) {
      total = event.complete().total();
    }
  }
  const auto status = reader->Finish();
  if (!status.ok()) {
    fail("RunQualityPass: " + status.error_message());
  }
  std::println("ok: RunQualityPass flagged={} total={}", flags, total);
}

// Runs RunSimilarPass, optionally excluding `skip_id`. Returns every image id
// that appeared in some emitted group.
std::vector<std::string> similar_pass_once(const options &opts,
                                           const std::string &skip_id,
                                           std::size_t &groups_out) {
  auto stub = k::Kustavi::NewStub(make_channel(opts));
  k::RunSimilarPassRequest request;
  if (!skip_id.empty()) {
    request.add_skip_image_ids(skip_id);
  }
  grpc::ClientContext context;
  add_auth_metadata(context, opts);
  auto reader = stub->RunSimilarPass(&context, request);
  k::SimilarEvent event;
  std::size_t groups = 0;
  uint32_t complete_groups = 0;
  std::vector<std::string> members;
  while (reader->Read(&event)) {
    if (event.has_group()) {
      groups++;
      if (event.group().image_ids_size() < 2) {
        fail("similar group with fewer than 2 members");
      }
      for (const auto &id : event.group().image_ids()) {
        members.push_back(id);
      }
    } else if (event.has_complete()) {
      complete_groups = event.complete().groups();
    }
  }
  const auto status = reader->Finish();
  if (!status.ok()) {
    fail("RunSimilarPass: " + status.error_message());
  }
  if (groups != complete_groups) {
    fail("similar group count mismatch: streamed " + std::to_string(groups) +
         " vs complete " + std::to_string(complete_groups));
  }
  groups_out = groups;
  return members;
}

void run_similar(const options &opts) {
  std::size_t groups = 0;
  const auto members = similar_pass_once(opts, "", groups);
  std::println("ok: RunSimilarPass groups={}", groups);

  if (members.empty()) {
    return; // nothing grouped in this folder; skip the exclusion check
  }

  // Re-run excluding one grouped image; it must not reappear, and every
  // emitted group must still have >= 2 members (enforced above).
  const std::string &skip_id = members.front();
  std::size_t groups_after = 0;
  const auto after = similar_pass_once(opts, skip_id, groups_after);
  for (const auto &id : after) {
    if (id == skip_id) {
      fail("skipped image " + skip_id + " reappeared in a similar group");
    }
  }
  std::println("ok: RunSimilarPass skip_image_ids honored (groups {} -> {})",
               groups, groups_after);
}

// Opt-in (`--junk-check`): exercise the vision pipeline. EnsureModel downloads
// the Qwen2.5-VL-3B GGUF weights (~3.3 GB, cached in the OS app-data dir on the
// first run), then RunJunkPass classifies every scanned image.
void run_ensure_model(const options &opts) {
  auto stub = k::Kustavi::NewStub(make_channel(opts));
  k::EnsureModelRequest request;
  grpc::ClientContext context;
  add_auth_metadata(context, opts);
  auto reader = stub->EnsureModel(&context, request);
  k::ModelEvent event;
  bool ready = false;
  std::uint64_t last_logged = 0;
  while (reader->Read(&event)) {
    if (event.has_ready()) {
      ready = true;
      std::println("ok: EnsureModel ready model={} size={}",
                   event.ready().model_name(), event.ready().size_bytes());
    } else if (event.has_progress()) {
      const auto done = event.progress().done_bytes();
      const auto total = event.progress().total_bytes();
      if (done >= last_logged + (128ULL << 20) ||
          (total > 0 && done == total)) {
        last_logged = done;
        std::println("   EnsureModel {} / {} bytes", done, total);
      }
    }
  }
  const auto status = reader->Finish();
  if (!status.ok()) {
    fail("EnsureModel: " + status.error_message());
  }
  if (!ready) {
    fail("EnsureModel: stream ended without a ready event");
  }
}

void run_junk(const options &opts) {
  auto stub = k::Kustavi::NewStub(make_channel(opts));
  k::RunJunkPassRequest request;
  grpc::ClientContext context;
  add_auth_metadata(context, opts);
  auto reader = stub->RunJunkPass(&context, request);
  k::JunkEvent event;
  std::size_t flags = 0;
  uint32_t complete_flagged = 0;
  uint32_t complete_total = 0;
  bool saw_complete = false;
  while (reader->Read(&event)) {
    if (event.has_flag()) {
      flags++;
      if (event.flag().image_id().empty()) {
        fail("JunkFlag with empty image_id");
      }
      std::println("   junk: {} reason={} confidence={:.2f}",
                   event.flag().image_id(), event.flag().reason(),
                   event.flag().confidence());
    } else if (event.has_complete()) {
      saw_complete = true;
      complete_flagged = event.complete().flagged();
      complete_total = event.complete().total();
    }
  }
  const auto status = reader->Finish();
  if (!status.ok()) {
    fail("RunJunkPass: " + status.error_message());
  }
  if (!saw_complete) {
    fail("RunJunkPass: stream ended without a complete event");
  }
  if (flags != complete_flagged) {
    fail("junk flag count mismatch: streamed " + std::to_string(flags) +
         " vs complete " + std::to_string(complete_flagged));
  }
  std::println("ok: RunJunkPass flagged={} total={}", flags, complete_total);
}

void run_trips(const options &opts) {
  auto stub = k::Kustavi::NewStub(make_channel(opts));
  k::RunTripsPassRequest request;
  grpc::ClientContext context;
  add_auth_metadata(context, opts);
  auto reader = stub->RunTripsPass(&context, request);
  k::TripsEvent event;
  std::size_t trips = 0;
  uint32_t complete_trips = 0;
  while (reader->Read(&event)) {
    if (event.has_trip()) {
      trips++;
    } else if (event.has_complete()) {
      complete_trips = event.complete().trips();
    }
  }
  const auto status = reader->Finish();
  if (!status.ok()) {
    fail("RunTripsPass: " + status.error_message());
  }
  if (trips != complete_trips) {
    fail("trips count mismatch: streamed " + std::to_string(trips) +
         " vs complete " + std::to_string(complete_trips));
  }
  std::println("ok: RunTripsPass trips={}", trips);
}

void run_commit(const options &opts) {
  auto stub = k::Kustavi::NewStub(make_channel(opts));
  k::CommitRequest request;
  request.set_destination(opts.destination);
  for (const auto &id : g_image_ids) {
    request.add_keep_ids(id);
  }
  grpc::ClientContext context;
  add_auth_metadata(context, opts);
  auto reader = stub->Commit(&context, request);
  k::CommitEvent event;
  uint32_t copied = 0;
  uint32_t skipped = 0;
  while (reader->Read(&event)) {
    if (event.has_complete()) {
      copied = event.complete().copied();
      skipped = event.complete().skipped();
    }
  }
  const auto status = reader->Finish();
  if (!status.ok()) {
    fail("Commit: " + status.error_message());
  }
  if (copied != static_cast<uint32_t>(g_image_ids.size()) || skipped != 0) {
    fail("Commit copied " + std::to_string(copied) + " skipped " +
         std::to_string(skipped) + " of " + std::to_string(g_image_ids.size()));
  }

  // Idempotency: re-committing the same set (identical sizes) still copies.
  grpc::ClientContext second_context;
  add_auth_metadata(second_context, opts);
  auto second_reader = stub->Commit(&second_context, request);
  k::CommitEvent second_event;
  uint32_t second_copied = 0;
  while (second_reader->Read(&second_event)) {
    if (second_event.has_complete()) {
      second_copied = second_event.complete().copied();
    }
  }
  const auto second_status = second_reader->Finish();
  if (!second_status.ok()) {
    fail("Commit re-run: " + second_status.error_message());
  }
  if (second_copied != copied) {
    fail("Commit re-run copied " + std::to_string(second_copied) + " vs " +
         std::to_string(copied));
  }
  std::println("ok: Commit copied={} (re-run consistent)", copied);

  // Trip-folder layout: folder_for_id routes every kept file under one
  // sub-directory of a fresh destination.
  const std::filesystem::path foldered_dest =
      std::filesystem::path(opts.destination) / "foldered";
  k::CommitRequest foldered;
  foldered.set_destination(foldered_dest.string());
  auto &id_to_folder = *foldered.mutable_folder_for_id();
  for (const auto &id : g_image_ids) {
    foldered.add_keep_ids(id);
    id_to_folder[id] = "trip-a";
  }
  grpc::ClientContext third_context;
  add_auth_metadata(third_context, opts);
  auto third_reader = stub->Commit(&third_context, foldered);
  k::CommitEvent third_event;
  uint32_t third_copied = 0;
  while (third_reader->Read(&third_event)) {
    if (third_event.has_complete()) {
      third_copied = third_event.complete().copied();
    }
  }
  const auto third_status = third_reader->Finish();
  if (!third_status.ok()) {
    fail("Commit foldered: " + third_status.error_message());
  }
  if (third_copied != static_cast<uint32_t>(g_image_ids.size())) {
    fail("Commit foldered copied " + std::to_string(third_copied));
  }
  std::error_code count_ec;
  std::size_t under_trip_a = 0;
  for (auto it = std::filesystem::recursive_directory_iterator(
           foldered_dest / "trip-a", count_ec);
       !count_ec && it != std::filesystem::recursive_directory_iterator();
       ++it) {
    if (it->is_regular_file()) {
      ++under_trip_a;
    }
  }
  if (under_trip_a != g_image_ids.size()) {
    fail("Commit foldered: " + std::to_string(under_trip_a) +
         " files under trip-a/, expected " +
         std::to_string(g_image_ids.size()));
  }
  std::println("ok: Commit folder_for_id routed {} files under trip-a/",
               under_trip_a);
}

void run_concurrency_check(const options &opts) {
  auto stub = k::Kustavi::NewStub(make_channel(opts));
  k::RunQualityPassRequest q_request;
  q_request.set_blur_threshold(100.0);
  q_request.set_underexposed_threshold(0.3);
  q_request.set_overexposed_threshold(0.3);
  grpc::ClientContext q_context;
  add_auth_metadata(q_context, opts);
  auto q_reader = stub->RunQualityPass(&q_context, q_request);
  k::QualityEvent q_event;
  if (!q_reader->Read(&q_event)) {
    fail("concurrency check: quality stream ended immediately");
  }

  // The quality pass is now running; a second pass must be rejected.
  k::RunSimilarPassRequest s_request;
  grpc::ClientContext s_context;
  add_auth_metadata(s_context, opts);
  auto s_reader = stub->RunSimilarPass(&s_context, s_request);
  k::SimilarEvent s_event;
  const bool more = s_reader->Read(&s_event);
  const auto s_status = s_reader->Finish();
  if (more || s_status.error_code() != grpc::StatusCode::FAILED_PRECONDITION) {
    fail("concurrency check: second pass was not rejected (status " +
         std::to_string(static_cast<int>(s_status.error_code())) + ")");
  }

  // Drain the quality stream to completion.
  while (q_reader->Read(&q_event)) {
  }
  if (!q_reader->Finish().ok()) {
    fail("concurrency check: quality pass did not complete cleanly");
  }
  std::println("ok: concurrent pass rejected with FAILED_PRECONDITION");
}

void run_cancel_check(const options &opts) {
  auto stub = k::Kustavi::NewStub(make_channel(opts));
  k::ScanFolderRequest request;
  request.set_folder(opts.folder);
  request.set_recursive(true);
  grpc::ClientContext context;
  add_auth_metadata(context, opts);
  auto reader = stub->ScanFolder(&context, request);
  k::ScanEvent event;
  // Read one event (the pass is now in flight), then cancel the stream.
  if (!reader->Read(&event)) {
    fail("cancel check: scan stream ended before we could cancel");
  }
  context.TryCancel();
  const auto start = std::chrono::steady_clock::now();
  while (reader->Read(&event)) {
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const auto status = reader->Finish();
  if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() > 10) {
    fail("cancel check: server took too long to wind down the pass");
  }
  std::println(
      "ok: cancelled scan wound down in {} ms (finish={})",
      static_cast<long long>(
          std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
              .count()),
      static_cast<int>(status.error_code()));

  // The pass lock must be released: a fresh scan works. Retry briefly while
  // the cancelled pass finishes winding down on the server.
  for (int attempt = 0; attempt < 200; ++attempt) {
    k::ScanFolderRequest retry_request;
    retry_request.set_folder(opts.folder);
    retry_request.set_recursive(true);
    grpc::ClientContext retry_context;
    add_auth_metadata(retry_context, opts);
    auto retry_reader = stub->ScanFolder(&retry_context, retry_request);
    k::ScanEvent retry_event;
    std::size_t images = 0;
    while (retry_reader->Read(&retry_event)) {
      if (retry_event.has_image()) {
        images++;
      }
    }
    const auto retry_status = retry_reader->Finish();
    if (retry_status.error_code() == grpc::StatusCode::FAILED_PRECONDITION) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    if (!retry_status.ok()) {
      fail("cancel check: post-cancel scan failed: " +
           retry_status.error_message());
    }
    if (images == 0) {
      fail("cancel check: post-cancel scan returned no images");
    }
    std::println("ok: post-cancel scan works ({} images, attempt {})", images,
                 attempt + 1);
    return;
  }
  fail("cancel check: pass lock never released");
}

void run_shutdown(const options &opts) {
  auto stub = k::Kustavi::NewStub(make_channel(opts));
  k::ShutdownRequest request;
  k::ShutdownResponse response;
  grpc::ClientContext context;
  add_auth_metadata(context, opts);
  const auto status = stub->Shutdown(&context, request, &response);
  if (!status.ok()) {
    fail("Shutdown: " + status.error_message());
  }
  // The server must stop accepting connections shortly after Shutdown.
  for (int i = 0; i < 50; ++i) {
    grpc::ClientContext probe_context;
    k::GetInfoRequest probe_request;
    k::GetInfoResponse probe_response;
    const auto probe =
        stub->GetInfo(&probe_context, probe_request, &probe_response);
    if (probe.error_code() != grpc::StatusCode::OK) {
      std::println("ok: Shutdown (server stopped)");
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  fail("Shutdown: server still accepting requests");
}

} // namespace

auto main(int argc, char **argv) -> int {
  try {
    auto opts = parse_args(argc, argv);

    server_process server;
    if (opts.target.empty()) {
      // Spawn mode: run a local server for the duration of the checks.
      const auto binary = find_server_binary(opts);
      if (opts.token.empty()) {
        opts.token = generate_token();
      }
      auto spawned = spawn_server(binary, opts.token);
      if (!spawned) {
        fail("failed to spawn the server");
      }
      server = *spawned;
      g_server_pid = server.pid;
      std::atexit(reap_server_on_exit);
      ::signal(SIGINT, handle_termination_signal);
      ::signal(SIGTERM, handle_termination_signal);

      const auto port = wait_for_ready(server, std::chrono::seconds{60});
      if (!port) {
        fail("server did not report KUSTAVI-READY within 60s");
      }
      opts.target = "127.0.0.1:" + std::to_string(*port);
      std::println("server ready on {} (pid {})", opts.target, server.pid);
    } else if (opts.token.empty() && !opts.no_auth) {
      fail("--token is required when connecting to an external --target");
    }

    if (!opts.token.empty()) {
      check_get_info(opts, true);
    }
    check_get_info(opts, false);
    if (!opts.folder.empty()) {
      run_scan(opts);
      if (opts.junk_check) {
        run_ensure_model(opts);
        run_junk(opts);
      }
      run_quality(opts);
      run_similar(opts);
      run_trips(opts);
      if (!opts.destination.empty()) {
        std::error_code ec;
        fs::remove_all(opts.destination, ec);
        run_commit(opts);
      }
      if (opts.concurrency_check) {
        run_concurrency_check(opts);
      }
      if (opts.cancel_check) {
        run_cancel_check(opts);
      }
    } else {
      std::println("ok: no --folder given; skipping pass checks");
    }
    // In spawn mode the shutdown check always runs: the server must go away.
    if (opts.shutdown || server.pid > 0) {
      run_shutdown(opts);
    }
    std::println("ALL OK");

    if (server.pid > 0) {
      teardown_server(server, opts);
    }
    return 0;
  } catch (const std::exception &e) {
    // Plain fprintf: a catch handler must not itself throw.
    // NOLINTNEXTLINE(modernize-use-std-print)
    std::fprintf(stderr, "FAIL: unhandled exception: %s\n", e.what());
    std::exit(k_fail);
  } catch (...) {
    // NOLINTNEXTLINE(modernize-use-std-print)
    std::fprintf(stderr, "FAIL: unhandled unknown exception\n");
    std::exit(k_fail);
  }
}
