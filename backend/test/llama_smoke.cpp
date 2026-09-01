// Smoke test: initialize llama.cpp / ggml and report the compute devices the
// selected backend discovered. Verifies that the vendored llama.cpp links,
// loads, and picks up hardware acceleration on each platform (Metal on macOS).

#include <llama.h>

#include <ggml-backend.h>

#include <cstdlib>
#include <print>
#include <string_view>

namespace {

constexpr int k_fail = 1;

// ggml exposes a function named `ggml_backend_dev_type`, which hides the
// same-named enum; the `enum` tag is required to name the type.
auto device_type_name(enum ggml_backend_dev_type type) -> std::string_view {
  switch (type) {
  case GGML_BACKEND_DEVICE_TYPE_CPU:
    return "CPU";
  case GGML_BACKEND_DEVICE_TYPE_GPU:
    return "GPU";
  case GGML_BACKEND_DEVICE_TYPE_ACCEL:
    return "ACCEL";
  default:
    return "UNKNOWN";
  }
}

} // namespace

auto main() -> int {
  // Picks up any dynamically loadable ggml backends; a no-op for a statically
  // linked backend but always safe to call.
  ggml_backend_load_all();
  llama_backend_init();

  const size_t count = ggml_backend_dev_count();
  std::println("llama.cpp — {} compute device(s)", count);

  bool saw_gpu = false;
  for (size_t i = 0; i < count; ++i) {
    ggml_backend_dev_t dev = ggml_backend_dev_get(i);
    const auto type = ggml_backend_dev_type(dev);
    saw_gpu = saw_gpu || type == GGML_BACKEND_DEVICE_TYPE_GPU;
    std::println("  [{}] {:<6} {} — {}", i, device_type_name(type),
                 ggml_backend_dev_name(dev), ggml_backend_dev_description(dev));
  }

  llama_backend_free();

  if (count == 0) {
    std::println(stderr, "FAIL: no ggml compute devices registered");
    return k_fail;
  }

#ifdef __APPLE__
  if (!saw_gpu) {
    std::println(stderr, "FAIL: expected a Metal GPU device on macOS");
    return k_fail;
  }
#endif

  std::println("OK");
  return EXIT_SUCCESS;
}
