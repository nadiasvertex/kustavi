#include "process/child_process.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <utility>

namespace kustavi::process {

namespace {

auto last_error_text(char const *what) -> std::string {
  return std::string{what} + " failed with error " + std::to_string(::GetLastError());
}

auto to_wide(std::string const &text) -> std::wstring {
  if (text.empty()) {
    return {};
  }
  int const size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
  return wide;
}

// Quotes one argument so that `CommandLineToArgvW` and the C runtime read it back unchanged:
// backslashes before a quote are doubled and the quote is escaped.
auto quote_argument(std::wstring const &arg) -> std::wstring {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    return arg;
  }
  std::wstring out = L"\"";
  for (auto it = arg.begin();; ++it) {
    std::size_t backslashes = 0;
    while (it != arg.end() && *it == L'\\') {
      ++it;
      ++backslashes;
    }
    if (it == arg.end()) {
      out.append(backslashes * 2, L'\\');
      break;
    }
    if (*it == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
    } else {
      out.append(backslashes, L'\\');
    }
    out.push_back(*it);
  }
  out.push_back(L'"');
  return out;
}

struct handle {
  HANDLE value = nullptr;

  handle() = default;
  explicit handle(HANDLE h) : value(h) {}
  handle(handle &&other) noexcept : value(std::exchange(other.value, nullptr)) {}
  auto operator=(handle &&other) noexcept -> handle & {
    reset();
    value = std::exchange(other.value, nullptr);
    return *this;
  }
  handle(handle const &) = delete;
  auto operator=(handle const &) -> handle & = delete;
  ~handle() { reset(); }

  auto reset() -> void {
    if (value != nullptr && value != INVALID_HANDLE_VALUE) {
      ::CloseHandle(value);
    }
    value = nullptr;
  }
};

struct pipe_ends {
  handle read_end;
  handle write_end;
};

// The write end is inherited by the child. The read end is not.
auto make_pipe() -> std::expected<pipe_ends, std::string> {
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
  HANDLE read = nullptr;
  HANDLE write = nullptr;
  if (!::CreatePipe(&read, &write, &attributes, 0)) {
    return std::unexpected(last_error_text("CreatePipe"));
  }
  pipe_ends ends{handle{read}, handle{write}};
  ::SetHandleInformation(ends.read_end.value, HANDLE_FLAG_INHERIT, 0);
  return ends;
}

auto read_from(HANDLE pipe, std::span<char> buffer) -> std::ptrdiff_t {
  if (pipe == nullptr) {
    return 0;
  }
  DWORD count = 0;
  if (!::ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
    return ::GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
  }
  return static_cast<std::ptrdiff_t>(count);
}

} // namespace

struct child_process::impl {
  handle process;
  DWORD pid = 0;
  handle stdout_read;
  handle stderr_read;
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
  SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
  handle null_input{::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (null_input.value == INVALID_HANDLE_VALUE) {
    null_input.value = nullptr;
    return std::unexpected(last_error_text("CreateFileW(NUL)"));
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = null_input.value;
  startup.hStdOutput = out->write_end.value;
  startup.hStdError =
      options.capture_stderr ? err->write_end.value : ::GetStdHandle(STD_ERROR_HANDLE);

  std::wstring command = quote_argument(options.binary.wstring());
  for (auto const &arg : options.args) {
    command += L' ';
    command += quote_argument(to_wide(arg));
  }

  PROCESS_INFORMATION info{};
  if (!::CreateProcessW(options.binary.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) {
    return std::unexpected(last_error_text("CreateProcessW"));
  }
  ::CloseHandle(info.hThread);

  auto state = std::make_unique<impl>();
  state->process = handle{info.hProcess};
  state->pid = info.dwProcessId;
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
  return read_from(impl_->stdout_read.value, buffer);
}

auto child_process::read_stderr(std::span<char> buffer) -> std::ptrdiff_t {
  return read_from(impl_->stderr_read.value, buffer);
}

auto child_process::wait() -> int {
  ::WaitForSingleObject(impl_->process.value, INFINITE);
  DWORD code = 0;
  ::GetExitCodeProcess(impl_->process.value, &code);
  return static_cast<int>(code);
}

auto child_process::terminate() -> void { kill(); }

auto child_process::kill() -> void { ::TerminateProcess(impl_->process.value, 1); }

auto child_process::current_executable() -> std::filesystem::path {
  std::wstring buffer(MAX_PATH, L'\0');
  while (true) {
    DWORD const length = ::GetModuleFileNameW(nullptr, buffer.data(),
                                              static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      return {};
    }
    if (length < buffer.size()) {
      buffer.resize(length);
      return std::filesystem::path{buffer};
    }
    buffer.resize(buffer.size() * 2);
  }
}

auto child_process::current_process_id() -> std::int64_t { return ::GetCurrentProcessId(); }

} // namespace kustavi::process
