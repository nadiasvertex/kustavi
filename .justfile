os_name := os()
flutter_target := if os_name == "macos" { "macos" } else if os_name == "windows" { "windows" } else { "linux" }
ui_binary_path := if os_name == "macos" { "build/macos/Build/Products/Release/frontend.app/Contents/MacOS/frontend" } else if os_name == "windows" { "build/windows/x64/runner/Release/frontend.exe" } else { "build/linux/x64/release/bundle/frontend" }

build:
  bazel build //...

build-server:
  bazel build //backend:server

build-gui:
  bazel build //frontend:kustavi

run:
  cd frontend && flutter build {{flutter_target}}
  cd frontend && ./{{ui_binary_path}}
