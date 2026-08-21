#!/usr/bin/env just --justfile

CLANG_TIDY := "/opt/homebrew/opt/llvm/bin/clang-tidy"
CLANG_FORMAT := "/opt/homebrew/opt/llvm/bin/clang-format"

os_name := os()
flutter_target := if os_name == "macos" { "macos" } else if os_name == "windows" { "windows" } else { "linux" }
ui_binary_path := if os_name == "macos" { "build/macos/Build/Products/Release/frontend.app/Contents/MacOS/frontend" } else if os_name == "windows" { "build/windows/x64/runner/Release/frontend.exe" } else { "build/linux/x64/release/bundle/frontend" }

build:
  bazel build //...

build-release:
  bazel build //...--config=release

build-server:
  bazel build //backend:server

build-gui:
  bazel build //frontend:kustavi

proto:
  mkdir -p frontend/lib/src/generated && \
  tmp="$(mktemp -d)" && \
  mkdir "$tmp/kustavi" && \
  cp proto/service.proto "$tmp/kustavi/" && \
  PATH="$HOME/.pub-cache/bin:$PATH" protoc -I "$tmp" --dart_out="grpc:frontend/lib/src/generated" kustavi/service.proto && \
  rm -rf "$tmp"

run:
  bazel build //...
  bazel run //frontend:kustavi

compile-commands:
  bazel run :refresh_compile_commands

compile-commands-all:
  bazel run @hedron_compile_commands//:refresh_all

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
