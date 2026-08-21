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
