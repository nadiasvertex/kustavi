#include "app/executable_path.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

#include <vector>

namespace kustavi::gui {

auto current_executable(char const *argv0) -> std::filesystem::path {
  std::error_code error;
#if defined(_WIN32)
  std::wstring buffer(MAX_PATH, L'\0');
  while (true) {
    auto const length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      break;
    }
    if (length < buffer.size()) {
      buffer.resize(length);
      return buffer;
    }
    buffer.resize(buffer.size() * 2);
  }
#elif defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size);
  if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
    auto resolved = std::filesystem::canonical(buffer.data(), error);
    if (!error) {
      return resolved;
    }
  }
#else
  auto resolved = std::filesystem::read_symlink("/proc/self/exe", error);
  if (!error) {
    return resolved;
  }
#endif
  return std::filesystem::absolute(argv0, error);
}

auto current_process_id() -> long long {
#if defined(_WIN32)
  return static_cast<long long>(GetCurrentProcessId());
#else
  return static_cast<long long>(getpid());
#endif
}

} // namespace kustavi::gui
