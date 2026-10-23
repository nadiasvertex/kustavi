#include "process/child_process.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <system_error>
#include <utility>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char **environ;

namespace kustavi::process {

namespace {

struct descriptor {
  int fd = -1;

  descriptor() = default;
  explicit descriptor(int value) : fd(value) {}
  descriptor(descriptor &&other) noexcept : fd(std::exchange(other.fd, -1)) {}
  auto operator=(descriptor &&other) noexcept -> descriptor & {
    reset();
    fd = std::exchange(other.fd, -1);
    return *this;
  }
  descriptor(descriptor const &) = delete;
  auto operator=(descriptor const &) -> descriptor & = delete;
  ~descriptor() { reset(); }

  auto reset() -> void {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }
};

struct pipe_ends {
  descriptor read_end;
  descriptor write_end;
};

// The read end belongs to the parent and must not leak into the child, so both ends close on exec
// and the child gets its own copy of the write end through `dup2`.
auto make_pipe() -> std::expected<pipe_ends, std::string> {
  int fds[2];
  if (::pipe(fds) != 0) {
    return std::unexpected(std::string{"pipe failed: "} + std::strerror(errno));
  }
  pipe_ends ends{descriptor{fds[0]}, descriptor{fds[1]}};
  ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  return ends;
}

auto read_from(int fd, std::span<char> buffer) -> std::ptrdiff_t {
  if (fd < 0) {
    return 0;
  }
  while (true) {
    auto const count = ::read(fd, buffer.data(), buffer.size());
    if (count >= 0 || errno != EINTR) {
      return count;
    }
  }
}

} // namespace

struct child_process::impl {
  pid_t pid = -1;
  descriptor stdout_read;
  descriptor stderr_read;
  // Set once `wait` has collected the child, after which its pid may belong to another process.
  std::atomic<bool> reaped{false};
};

child_process::child_process(std::unique_ptr<impl> state) : impl_(std::move(state)) {}
child_process::child_process(child_process &&) noexcept = default;
auto child_process::operator=(child_process &&) noexcept -> child_process & = default;
child_process::~child_process() = default;

auto child_process::start(spawn_options const &options)
    -> std::expected<child_process, std::string> {
  auto out = make_pipe();
  if (!out) {
    return std::unexpected(out.error());
  }
  std::expected<pipe_ends, std::string> err = pipe_ends{};
  if (options.capture_stderr) {
    err = make_pipe();
    if (!err) {
      return std::unexpected(err.error());
    }
  }

  posix_spawn_file_actions_t actions;
  if (::posix_spawn_file_actions_init(&actions) != 0) {
    return std::unexpected("posix_spawn_file_actions_init failed");
  }
  bool ok = ::posix_spawn_file_actions_adddup2(&actions, out->write_end.fd, 1) == 0 &&
            ::posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0) == 0;
  if (options.capture_stderr) {
    ok = ok && ::posix_spawn_file_actions_adddup2(&actions, err->write_end.fd, 2) == 0;
  }
  if (!ok) {
    ::posix_spawn_file_actions_destroy(&actions);
    return std::unexpected("posix_spawn_file_actions setup failed");
  }

  // The strings must outlive the spawn call, because argv points into them.
  std::vector<std::string> command{options.binary.string()};
  command.insert(command.end(), options.args.begin(), options.args.end());
  std::vector<char *> argv;
  for (auto &arg : command) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);

  pid_t pid = -1;
  int const status =
      ::posix_spawn(&pid, command[0].c_str(), &actions, nullptr, argv.data(), environ);
  ::posix_spawn_file_actions_destroy(&actions);
  if (status != 0) {
    return std::unexpected(std::string{"posix_spawn failed: "} + std::strerror(status));
  }

  auto state = std::make_unique<impl>();
  state->pid = pid;
  out->write_end.reset();
  state->stdout_read = std::move(out->read_end);
  if (options.capture_stderr) {
    err->write_end.reset();
    state->stderr_read = std::move(err->read_end);
  }
  return child_process{std::move(state)};
}

auto child_process::id() const -> std::int64_t { return impl_->pid; }

auto child_process::read_stdout(std::span<char> buffer) -> std::ptrdiff_t {
  return read_from(impl_->stdout_read.fd, buffer);
}

auto child_process::read_stderr(std::span<char> buffer) -> std::ptrdiff_t {
  return read_from(impl_->stderr_read.fd, buffer);
}

auto child_process::wait() -> int {
  int status = 0;
  while (::waitpid(impl_->pid, &status, 0) < 0) {
    if (errno != EINTR) {
      impl_->reaped = true;
      return -1;
    }
  }
  impl_->reaped = true;
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
}

auto child_process::terminate() -> void {
  if (!impl_->reaped) {
    ::kill(impl_->pid, SIGTERM);
  }
}

auto child_process::kill() -> void {
  if (!impl_->reaped) {
    ::kill(impl_->pid, SIGKILL);
  }
}

auto child_process::current_executable() -> std::filesystem::path {
  std::vector<char> buffer(4096);
#ifdef __APPLE__
  auto size = static_cast<std::uint32_t>(buffer.size());
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    return {};
  }
#else
  auto const length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (length < 0) {
    return {};
  }
  buffer[static_cast<std::size_t>(length)] = '\0';
#endif
  std::filesystem::path path{buffer.data()};
  std::error_code error;
  if (auto canonical = std::filesystem::canonical(path, error); !error) {
    return canonical;
  }
  return path;
}

auto child_process::current_process_id() -> std::int64_t { return ::getpid(); }

} // namespace kustavi::process
