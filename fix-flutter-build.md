# Fix `frontend/BUILD.bazel`: build the Flutter app via Bazel-native rules

Goal: `bazel build //frontend:kustavi` builds the FLUTTER app in `frontend/`
for whatever platform Bazel is currently targeting (host = target for
desktop; routes to the macOS / Linux / Windows bundle based on host OS).

## Decisions (confirmed)

- Integration: `rules_flutter` (Bazel module, hermetic Flutter SDK).
- Platforms: desktop only (macOS, Linux, Windows). No web/mobile.
- macOS artifact: release `.app` bundle.

## Findings

- Current `frontend/BUILD.bazel` is a broken genrule sketch: hardcoded
  `flutter build macos`, bogus `PATH` entry, `cd frontend` (genrules run in
  execroot, not the source tree), non-hermetic `local = 1`.
- Selected ruleset: `github.com/aran/rules_flutter` — Flutter-team rules:
  Bazel-native kernel compile → AOT (`gen_snapshot`) → platform bundles.
  Tier-1 macros: `flutter_macos_app`, `flutter_linux_app`,
  `flutter_windows_app`. Complete e2e examples (`e2e/hello_world` is
  structurally identical to our frontend: counter app + widget test).
- Status: v0.0.1 alpha; public API may change before 1.0.
- Not yet on the BCR (`registry.bazel.build` has no entry) → pin a commit
  with `git_override`. Requires Bazel 9+ (repo has 9.2.0).
- Ships its own hermetic Flutter SDK, pinned 3.44.1 (engine artifacts
  downloaded on first build). The local Flutter 3.47.1 install is not used.
- From the e2e consumer `MODULE.bazel`:
  - consumers also pin `bazel_dep(name = "rules_dart", version = "0.4.9")`
    — this is rules_flutter's fork, not upstream `dart-lang/rules_dart`.
    Our root currently pins `rules_dart 1.0.0` and would conflict; nothing
    else in the repo references `rules_dart`, so swap it.
  - required `.bazelrc` (JDK bump for transitive rules_jvm_external):
    ```
    common --tool_java_language_version=25
    common --tool_java_runtime_version=remotejdk_25
    ```
  - pub deps come from `flutter.pub(name = "deps", lock = "//:pubspec.lock")`
    in the same `flutter` extension as `flutter.toolchain(...)`; packages
    are referenced as `@deps//:pkg` (e.g. `@deps//:flutter`,
    `@deps//:cupertino_icons`).
- `.justfile` already expects `bazel build //frontend:kustavi` — keep that
  target name.
- Bazel 9 `alias` supports `select()` in `actual`; use that for the
  `:kustavi` host-router. Fallback if rejected: a tiny forwarding rule, or
  drop the alias and update `.justfile` to `kustavi_macos` etc.

## Changes

### 1. `MODULE.bazel` (root)

- Remove the `bazel_dep(name = "rules_dart", version = "1.0.0")` block
  ("Flutter / Dart rules integration" comment).
- Add:
  ```starlark
  bazel_dep(name = "rules_dart", version = "0.4.9")

   bazel_dep(name = "rules_flutter", version = "0.0.0")
   git_override(
       module_name = "rules_flutter",
       remote = "https://github.com/aran/rules_flutter.git",
       commit = "cfb2921cb2129d0658ba195396ecd3f1c97aa997",  # main @ 2026-08-21
       patches = ["//patches:rules_flutter-subdir-app.patch"],
   )

  flutter = use_extension("@rules_flutter//flutter:extensions.bzl", "flutter")
  flutter.toolchain(flutter_version = "3.44.1")
  flutter.pub(name = "deps", lock = "//frontend:pubspec.lock")
  use_repo(flutter, "deps", "flutter_toolchains", "<needed engine repos>")
  register_toolchains("@flutter_toolchains//:all")
  ```
- Pin `<PIN>` from `aran/rules_flutter@main` (record the commit hash here
  when done). Inspect `use_repo` names in `extensions.bzl` and keep only
  repos our (desktop-only, no Android/iOS) config actually uses.
- Run `bazel mod tidy` after wiring.

### 2. New `.bazelrc`

```
common --tool_java_language_version=25
common --tool_java_runtime_version=remotejdk_25
startup:windows --windows_enable_symlinks
```

### 3. Rewrite `frontend/BUILD.bazel`

- `exports_files(["pubspec.lock"])`
- Shared core (exact `deps` list derived from the direct deps in
  `frontend/pubspec.yaml` / `pubspec.lock` — expected: `@deps//:flutter`,
  `@deps//:cupertino_icons`, `@rules_flutter//flutter:material_icons`;
  `@deps//:flutter_localized_locales` only if a transitive font package
  demands it in generation):
  ```starlark
  flutter_application(
      name = "kustavi_app",
      package_name = "kustavi",  # must match pubspec.yaml name:
      main = "lib/main.dart",
      srcs = glob(["lib/**/*.dart"]),
      deps = [...],
  )
  ```
- One Tier-1 target per desktop OS, host-pinned (e2e convention; on a given
  host only that host's target is compatible, so "build the platform Bazel
  is targeting" = plain `bazel build` / `//frontend:all`):
  - `flutter_macos_app(name = "kustavi_macos", application = ":kustavi_app",
    bundle_id = "dev.kustavi.app", app_name = "Kustavi",
    target_compatible_with = ["@platforms//os:macos"])`
  - `flutter_linux_app(name = "kustavi_linux", application = ":kustavi_app",
    gtk_app_id = "com.kustavi.desktop",
    target_compatible_with = ["@platforms//os:linux"])`
    (cross-build on non-Linux host: only `-c dbg` +
    `--platforms=@rules_flutter//flutter/platforms:linux_x64` — Flutter
    publishes no cross-`gen_snapshot` for desktop targets.)
  - `flutter_windows_app(name = "kustavi_windows", application = ":kustavi_app",
    target_compatible_with = ["@platforms//os:windows"])`
- Host router (keeps `.justfile` working):
  ```starlark
  alias(
      name = "kustavi",
      actual = select({
          "@platforms//os:macos": ":kustavi_macos",
          "@platforms//os:linux": ":kustavi_linux",
          "@platforms//os:windows": ":kustavi_windows",
      }),
  )
  ```
- Optional, non-blocking: `flutter_test(name = "widget_test", ...)` for
  `test/widget_test.dart`, mirroring e2e (`main = "test/widget_test.dart"`,
  deps `@deps//:flutter_test` etc.).
- Remove the old genrule.

### 4. `pubspec.lock` hygiene (conditional)

The checked-in lock was resolved by Dart ≥ 3.14 (host Flutter 3.47.1);
rules_flutter pins SDK 3.44.1. If resolution fails against that SDK,
refresh with the hermetic toolchain: `bazel run @rules_flutter//flutter:pub
-- get` (do NOT use the host `flutter pub`), and commit the new lock.

### 5. Verification

1. `bazel build //frontend:kustavi -c opt` (first run downloads engine
   artifacts — multiple minutes, one-time).
2. Inspect `bazel-bin/frontend/`: expect the macOS bundle (per e2e, a zip
   containing `Kustavi.app` with `Contents/MacOS/`, `Frameworks/`,
   `flutter_assets/`). Unzip and smoke-run the executable.
3. `bazel build //frontend:all -c opt` → only `kustavi_macos` compatible on
   this host.
4. `bazel build //backend:server` — confirm the `MODULE.bazel` swap
   didn't regress the C++ side.
5. `bazel test //frontend:widget_test` (if added).
6. `bazel build //... -c opt` end-to-end.

## Implementation results (2026-08-21)

- Pinned commit: `cfb2921cb2129d0658ba195396ecd3f1c97aa997` (main @ 2026-08-21).
- `rules_dart`: the root had `0.4.10` (not `1.0.0`), but both are the
  `aran/rules_dart` fork on the BCR; `0.4.10` removed
  `DartInfo.transitive_resources`, which rules_flutter's generated pub spokes
  use, so it was downgraded to `0.4.9` (matches `rules_dart_proto`'s
  requirement too).
- `bazel_dep(name = "platforms", ...)` added to the root module: `@platforms`
  is not visible to the main module's BUILD files without a direct dep.
- `use_repo` keeps only the desktop subset actually consumed here:
  `deps`, `flutter_dev_root`, `flutter_macos_engine`, `flutter_toolchains`.
  Add `flutter_linux_engine` / `flutter_windows_engine` on their host OSes.
- New patch `patches/rules_flutter-subdir-app.patch`: at this commit the
  ruleset synthesizes the app's own Dart package with `lib_root=""`
  (workspace root), so a Flutter app in a subdirectory (our `frontend/`)
  gets a `package_config.json` pointing at the wrong root. The patch adds an
  optional `lib_root` to `synthesize_app_package` (default `""`, so
  root-directory apps and the ruleset's unit tests are unaffected) and passes
  `derive_lib_root(ctx.label.workspace_root, ctx.label.package)` at the
  `flutter_application` / `flutter_test` / `flutter_web_application` call
  sites. Verified against the ruleset's own `//flutter/tests:common_test`
  (8/8 pass) and our `frontend/` build in `-c dbg`, fastbuild, and `-c opt`.
- Lock refresh was NOT needed: Flutter 3.44.1's Dart satisfies the checked-in
  `pubspec.lock` floors (dart `>=3.13.1`, flutter `>=3.18.0`).
- Verified: `bazel build //frontend:kustavi` (alias → `kustavi_macos`),
  `//frontend:all -c opt`, `//backend:server`, `//...` all build; the bundle
  is `bazel-bin/frontend/kustavi_macos.zip` containing `Kustavi.app`
  (signed, `Contents/MacOS/Kustavi` runs).

## Risks / caveats

- Alpha ruleset (`v0.0.1`); API churn expected before 1.0. Mitigated by
  commit pin.
- First build: large one-time engine download from Google storage.
- macOS bundle is unsigned/ad-hoc (no signing configured), same as
  `flutter build macos`.
- Windows target is declared but only buildable on a Windows host/CI —
  cannot be verified from this machine.
- `rules_dart` fork (`0.4.9`) replaces upstream `1.0.0` in the module
  graph; if any future dependency needs upstream, re-visit with
  `single_version_override`.
