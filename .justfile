#!/usr/bin/env just --justfile

CLANG_TIDY := "/opt/homebrew/opt/llvm/bin/clang-tidy"
CLANG_FORMAT := "/opt/homebrew/opt/llvm/bin/clang-format"

PRESET := env_var_or_default("PRESET", "debug")

configure:
  cmake --preset {{ PRESET }}

build: configure
  cmake --build --preset {{ PRESET }}

build-release:
  cmake --preset release
  cmake --build --preset release

build-server-release:
  cmake --preset release
  cmake --build --preset release --target server

build-server:
  cmake --preset {{ PRESET }}
  cmake --build --preset {{ PRESET }} --target server

# Builds the Flutter GUI with the host's Flutter SDK. Goes away with the
# Flutter front end (spec/plans/cpp-gui-rewrite.md, phase 8).
build-gui:
  #!/usr/bin/env sh
  set -e
  case "$(uname -s)" in
    Darwin) target=macos ;;
    Linux) target=linux ;;
    *) target=windows ;;
  esac
  cd frontend && flutter build "$target" --release

test-gui:
  cd frontend && flutter test

# Back-end tests: unit tests plus the smoke test, where the smoke client spawns
# the server itself (ephemeral loopback port), exercises every RPC pass against
# a copy of test/photos, then shuts the server down.
test-backend: build
  ctest --preset {{ PRESET }} -L backend

# C++ GUI core tests: the pure logic, the gRPC client against an in-process
# server, and the process library.
test-gui-core: build
  ctest --preset {{ PRESET }} -L "gui|process"

# llama.cpp smoke test: links the vendored llama.cpp and prints the compute
# devices the platform backend discovered (Metal GPU on macOS).
test-llama: build
  ctest --preset {{ PRESET }} -L llama -V

# Vision (junk) pass end-to-end. Downloads the ~3.3 GB Qwen2.5-VL-3B weights on
# the first run (cached in the OS app-data dir), then runs EnsureModel +
# RunJunkPass against a copy of test/photos. Not part of `just test`.
test-junk: build
  ctest --preset {{ PRESET }} -L junk -V

# Video pass heuristics (duration/blur/motion/corruption) against small
# synthetic clips. Not part of `just test`; has no model download of its own
# (video_test never loads the vision model).
test-video: build
  ctest --preset {{ PRESET }} -L video -V

# Quality pass: synthetic blur heuristics plus the real-photo corpus in
# test/photos/ (blur/exposure classification + near-duplicate grouping). No
# model download. Not part of `just test`.
test-quality: build
  ctest --preset {{ PRESET }} -L quality -V

test: test-backend test-llama test-gui

proto:
  mkdir -p frontend/lib/src/generated && \
  tmp="$(mktemp -d)" && \
  mkdir "$tmp/kustavi" && \
  cp proto/service.proto "$tmp/kustavi/" && \
  PATH="$HOME/.pub-cache/bin:$PATH" protoc -I "$tmp" --dart_out="grpc:frontend/lib/src/generated" kustavi/service.proto && \
  rm -rf "$tmp"

# CMake writes .build/<preset>/compile_commands.json while configuring.
compile-commands:
  cmake --preset {{ PRESET }}

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
      | xargs -0 -P1 {{ CLANG_TIDY }} -p=.build/{{ PRESET }} --fix --fix-errors \
          --extra-arg=-isysroot --extra-arg="$SDK"

# Build a redistributable desktop package (zip) for the host OS. The build +
# bundle logic lives in tools/package.py, which also covers Windows. Extra
# args pass straight through, e.g. `just package --keep`.
package *ARGS:
  #!/usr/bin/env sh
  set -e
  for c in python3 python py; do "$c" -c "import sys" >/dev/null 2>&1 && PY=$c && break; done
  : "${PY:?no working python interpreter on PATH}"
  "$PY" tools/package.py {{ ARGS }}

# Build the Windows MSI installer (WiX v5; needs `dotnet tool install -g wix`).
# See tools/installer/README.md.
installer *ARGS:
  #!/usr/bin/env sh
  set -e
  for c in python3 python py; do "$c" -c "import sys" >/dev/null 2>&1 && PY=$c && break; done
  : "${PY:?no working python interpreter on PATH}"
  "$PY" tools/package.py --installer {{ ARGS }}

# Print the current app version (the repo-root VERSION.txt file).
show-version:
  #!/usr/bin/env sh
  set -e
  for c in python3 python py; do "$c" -c "import sys" >/dev/null 2>&1 && PY=$c && break; done
  : "${PY:?no working python interpreter on PATH}"
  "$PY" tools/version.py show

# Bump the global version and propagate it to the front end, back end,
# the CMake project and the Windows resource script. COMPONENT is major|minor|patch.
# Pass `--tag` through to also commit + tag, e.g. `just version-bump patch --tag`.
bump-version COMPONENT *ARGS:
  #!/usr/bin/env sh
  set -e
  for c in python3 python py; do "$c" -c "import sys" >/dev/null 2>&1 && PY=$c && break; done
  : "${PY:?no working python interpreter on PATH}"
  "$PY" tools/version.py bump {{ COMPONENT }} {{ ARGS }}

# Package, then launch the unpacked macOS app from dist/.
run:
  #!/usr/bin/env sh
  set -e
  for c in python3 python py; do "$c" -c "import sys" >/dev/null 2>&1 && PY=$c && break; done
  : "${PY:?no working python interpreter on PATH}"
  "$PY" tools/package.py --keep
  dist/Kustavi.app/Contents/MacOS/Kustavi
