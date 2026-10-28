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

# Builds the C++ GUI against the pinned Basalt and Magma. To use checkouts instead, point
# KUSTAVI_BASALT_SOURCE_DIR and KUSTAVI_MAGMA_SOURCE_DIR at them.
build-gui:
  #!/usr/bin/env sh
  set -e
  cmake --preset {{ PRESET }} -DKUSTAVI_BUILD_GUI=ON \
    ${KUSTAVI_BASALT_SOURCE_DIR:+-DKUSTAVI_BASALT_SOURCE_DIR="$KUSTAVI_BASALT_SOURCE_DIR"} \
    ${KUSTAVI_MAGMA_SOURCE_DIR:+-DKUSTAVI_MAGMA_SOURCE_DIR="$KUSTAVI_MAGMA_SOURCE_DIR"}
  cmake --build --preset {{ PRESET }} --target kustavi_gui

# Runs the GUI from the build tree.
run-gui: build-gui
  #!/usr/bin/env sh
  set -e
  ".build/{{ PRESET }}/bin/kustavi_gui"

# The GUI's tests: the logic, the screens, and the client against a real engine.
test-gui: build-gui
  ctest --preset {{ PRESET }} -L gui

# Back-end tests: unit tests plus the engine test, which runs every pass against
# a copy of test/photos.
test-backend: build
  ctest --preset {{ PRESET }} -L backend

# The GUI's core tests alone, which need no window library: the pure logic and the client
# against a real engine.
test-gui-core: build
  ctest --preset {{ PRESET }} -L gui

# llama.cpp smoke test: links the vendored llama.cpp and prints the compute
# devices the platform backend discovered (Metal GPU on macOS).
test-llama: build
  ctest --preset {{ PRESET }} -L llama -V

# Vision (junk) pass end-to-end. Downloads the ~3.3 GB Qwen2.5-VL-3B weights on
# the first run (cached in the OS app-data dir), then runs ensure_model +
# run_junk_pass against a copy of test/photos. Not part of `just test`.
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
    # Homebrew clang-tidy cannot read Apple clang's precompiled headers, so it reads a copy of the
    # compile commands without them; it still parses the same headers through -include.
    mkdir -p .build/{{ PRESET }}/tidy
    sed -E 's/ -Xclang -include-pch -Xclang [^ ]+//g' .build/{{ PRESET }}/compile_commands.json \
      > .build/{{ PRESET }}/tidy/compile_commands.json
    SDK="$(xcrun --show-sdk-path)"; \
    find ./backend -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.h" \) -print0 \
      | xargs -0 -P1 {{ CLANG_TIDY }} -p=.build/{{ PRESET }}/tidy --fix --fix-errors \
          --extra-arg=-isysroot --extra-arg="$SDK"

# Build a redistributable desktop package (zip) for the host OS. The build +
# bundle logic lives in tools/package.py, which also covers Windows. Extra
# args pass straight through, e.g. `just package --keep`. The build uses the
# pinned Basalt and Magma; set KUSTAVI_BASALT_SOURCE_DIR and KUSTAVI_MAGMA_SOURCE_DIR, or pass
# --basalt-dir and --magma-dir, to use checkouts.
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

# Bump the global version and propagate it to the back end. The CMake project reads it
# itself. COMPONENT is major|minor|patch.
# Commits the changed version files as "Release v<version>"; tagging is left to `just release`.
bump-version COMPONENT *ARGS:
  #!/usr/bin/env sh
  set -e
  for c in python3 python py; do "$c" -c "import sys" >/dev/null 2>&1 && PY=$c && break; done
  : "${PY:?no working python interpreter on PATH}"
  "$PY" tools/version.py bump {{ COMPONENT }} {{ ARGS }}
  version=$(tr -d '[:space:]' < VERSION.txt)
  git add VERSION.txt backend/include/version.h
  git commit -m "Release v$version" -- VERSION.txt backend/include/version.h

# Bump the version with `just bump-version patch` (which commits it) first.
# Tag HEAD as v<version> from VERSION.txt and push the tag, which starts the GitHub release build.
release:
  #!/usr/bin/env bash
  set -euo pipefail
  version=$(tr -d '[:space:]' < VERSION.txt)
  tag="v$version"
  if git rev-parse -q --verify "refs/tags/$tag" >/dev/null \
     || git ls-remote --exit-code --tags origin "refs/tags/$tag" >/dev/null 2>&1; then
    echo "Tag $tag already exists. Run 'just bump-version <major|minor|patch>' and commit the change first." >&2
    exit 1
  fi
  if ! git diff --quiet HEAD -- VERSION.txt backend/include/version.h; then
    echo "The version files have uncommitted changes. Commit the version bump before releasing." >&2
    exit 1
  fi
  git tag -a "$tag" -m "Kustavi $tag"
  git push origin "$tag"
  echo "Pushed $tag"

# Package, then launch the unpacked macOS app from dist/.
run:
  #!/usr/bin/env sh
  set -e
  for c in python3 python py; do "$c" -c "import sys" >/dev/null 2>&1 && PY=$c && break; done
  : "${PY:?no working python interpreter on PATH}"
  "$PY" tools/package.py --keep
  dist/Kustavi.app/Contents/MacOS/Kustavi
