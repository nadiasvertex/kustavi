#!/usr/bin/env just --justfile

CLANG_TIDY := "/opt/homebrew/opt/llvm/bin/clang-tidy"
CLANG_FORMAT := "/opt/homebrew/opt/llvm/bin/clang-format"

build:
  bazel build //...

build-release:
  bazel build //... --config=release

build-server-release:
  bazel build //backend:server --config=release

build-server:
  bazel build //backend:server

build-gui:
  bazel build //frontend:kustavi

test-gui:
  cd frontend && flutter test

# Backend smoke test: the smoke client spawns the server itself (ephemeral
# loopback port), exercises every RPC pass against a copy of test/photos,
# then shuts the server down.
test-backend:
  bazel build //backend:server //backend:smoke_client
  tmp="$(mktemp -d)" && \
  trap 'rm -rf "$tmp"' EXIT && \
  cp -R test/photos "$tmp/photos" && \
  bazel run //backend:smoke_client -- --folder "$tmp/photos" --destination "$tmp/committed" --concurrency-check --cancel-check

# llama.cpp smoke test: links the vendored llama.cpp and prints the compute
# devices the platform backend discovered (Metal GPU on macOS).
test-llama:
  bazel run //backend:llama_smoke

# Vision (junk) pass end-to-end. Downloads the ~3.3 GB Qwen2.5-VL-3B weights on
# the first run (cached in the OS app-data dir), then runs EnsureModel +
# RunJunkPass against a copy of test/photos. Not part of `just test`.
test-junk:
  bazel build //backend:server //backend:smoke_client
  tmp="$(mktemp -d)" && \
  trap 'rm -rf "$tmp"' EXIT && \
  cp -R test/photos "$tmp/photos" && \
  bazel run //backend:smoke_client -- --folder "$tmp/photos" --junk-check

test: test-backend test-llama test-gui

proto:
  mkdir -p frontend/lib/src/generated && \
  tmp="$(mktemp -d)" && \
  mkdir "$tmp/kustavi" && \
  cp proto/service.proto "$tmp/kustavi/" && \
  PATH="$HOME/.pub-cache/bin:$PATH" protoc -I "$tmp" --dart_out="grpc:frontend/lib/src/generated" kustavi/service.proto && \
  rm -rf "$tmp"

compile-commands:
  bazel run :refresh_compile_commands

compile-commands-all:
  bazel run @hedron_compile_commands//:refresh_all > /dev/null 2>&1

format:
    find ./backend -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.h" \) -print0 \
      | xargs -0 -P1 {{ CLANG_FORMAT }} -i

lint: compile-commands
    # Run clang-tidy
    # Auto-detect macOS SDK, then run clang-tidy with extra args.
    # clang-tidy resolves its own matching libc++ headers automatically; forcing
    # an extra -I for Homebrew LLVM's libc++ conflicts with the macOS SDK headers
    # (Apple clang vs. Homebrew clang) and corrupts parsing for every file.
    SDK="$(xcrun --show-sdk-path)"; \
    find ./backend -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.h" \) -print0 \
      | xargs -0 -P1 {{ CLANG_TIDY }} -p=. --fix --fix-errors \
          --extra-arg=-isysroot --extra-arg="$SDK"

dist-clean:
  rm -rf dist && mkdir -p dist

package-server: build-server-release
  @# Copy the backend binary → kustavi-backend
  cp bazel-bin/backend/server dist/kustavi-backend
  @# Bundle the llama.cpp shared libraries next to the binary and repoint the
  @# binary at them via an @loader_path rpath (Bazel's build rpath points into
  @# the sandbox and does not survive the copy).
  find -L bazel-bin/backend/server.runfiles \( -name 'libllama*.dylib' -o -name 'libggml*.dylib' -o -name 'libmtmd*.dylib' \) -type f \
    -exec cp {} dist/ \;
  install_name_tool -add_rpath @loader_path dist/kustavi-backend 2>/dev/null || true

package-gui: build-gui
  unzip -q bazel-bin/frontend/kustavi_macos.zip -d "dist/"
  mv dist/kustavi-backend dist/Kustavi.app/Contents/MacOS/
  @# Move the bundled llama.cpp dylibs alongside the backend binary.
  find dist -maxdepth 1 \( -name 'libllama*.dylib' -o -name 'libggml*.dylib' -o -name 'libmtmd*.dylib' \) \
    -exec mv {} dist/Kustavi.app/Contents/MacOS/ \;

package: dist-clean package-server package-gui

run: package
  dist/Kustavi.app/Contents/MacOS/Kustavi
