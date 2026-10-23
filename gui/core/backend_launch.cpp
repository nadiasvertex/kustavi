#include "core/backend_launch.h"

#include <format>
#include <random>

namespace kustavi::gui {

namespace {

constexpr std::string_view hex_digits = "0123456789abcdef";
constexpr std::string_view base36_digits = "0123456789abcdefghijklmnopqrstuvwxyz";

auto random_text(random_source const &random, std::string_view alphabet, std::size_t length)
    -> std::string {
  std::string text;
  text.reserve(length);
  for (std::size_t i = 0; i < length; ++i) {
    text.push_back(alphabet[random() % alphabet.size()]);
  }
  return text;
}

constexpr auto backend_name =
#ifdef _WIN32
    "kustavi-backend.exe";
#else
    "kustavi-backend";
#endif

} // namespace

auto system_random() -> random_source {
  auto engine = std::make_shared<std::random_device>();
  return [engine] { return (*engine)(); };
}

auto generate_auth_token(random_source const &random) -> std::string {
  return random_text(random, hex_digits, 64);
}

auto default_backend_log_file(std::filesystem::path const &temp_directory, civil_time now,
                              random_source const &random) -> std::filesystem::path {
  return temp_directory /
         std::format("kustavi-backend-{:04}{:02}{:02}-{:02}{:02}{:02}-{}.log", now.year,
                     now.month, now.day, now.hour, now.minute, now.second, random_text(random, base36_digits, 8));
}

auto find_backend_binary(std::optional<std::string> const &override_path,
                         std::filesystem::path const &executable)
    -> std::optional<std::filesystem::path> {
  if (override_path && !override_path->empty()) {
    std::filesystem::path const path{*override_path};
    return std::filesystem::is_regular_file(path) ? std::optional{path} : std::nullopt;
  }
  auto const candidate = executable.parent_path() / backend_name;
  return std::filesystem::is_regular_file(candidate) ? std::optional{candidate} : std::nullopt;
}

auto backend_arguments(std::string const &token,
                       std::optional<std::filesystem::path> const &log_file,
                       std::optional<std::int64_t> parent_pid) -> std::vector<std::string> {
  std::vector<std::string> args{"serve", "--listen", "127.0.0.1:0", "--token", token};
  if (log_file) {
    args.insert(args.end(), {"--log-file", log_file->string()});
  }
  if (parent_pid) {
    args.insert(args.end(), {"--parent-pid", std::to_string(*parent_pid)});
  }
  return args;
}

auto launch_backend(backend_launch options)
    -> std::expected<std::unique_ptr<backend_process>, backend_startup_failed> {
  process::launch_options launch;
  launch.spawn.binary = options.binary;
  launch.spawn.args = backend_arguments(options.token, options.log_file, options.parent_pid);
  launch.ready_timeout = options.ready_timeout;
  launch.on_exit = std::move(options.on_exit);
  auto process = process::supervised_process::launch(std::move(launch));
  if (!process) {
    return std::unexpected(backend_startup_failed{"Back end: " + process.error()});
  }
  return std::make_unique<backend_process>(std::move(*process), std::move(options.token),
                                           std::move(options.log_file));
}

} // namespace kustavi::gui
