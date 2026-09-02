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
  bazel build //backend:server //backend:smoke_client //backend:trips_test
  KUSTAVI_GEO_DATA="$(pwd)/backend/data/cities.tsv" bazel run //backend:trips_test
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

# Print the current app version (the repo-root VERSION file).
version-show:
  #!/usr/bin/env sh
  set -e
  for c in python3 python py; do "$c" -c "import sys" >/dev/null 2>&1 && PY=$c && break; done
  : "${PY:?no working python interpreter on PATH}"
  "$PY" tools/version.py show

# Bump the global version and propagate it to the front end, back end,
# MODULE.bazel and the Windows resource script. COMPONENT is major|minor|patch.
# Pass `--tag` through to also commit + tag, e.g. `just version-bump patch --tag`.
version-bump COMPONENT *ARGS:
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
