# Bazel-native overlay for `package:fvp` 0.x (macOS only for now).
#
# fvp's macOS/iOS native code links against `mdk` — a separate CocoaPod
# vendoring a prebuilt `mdk.xcframework` — declared via `s.dependency 'mdk'`
# in fvp's podspec (see `darwin/fvp.podspec`). Flutter's own build fetches it
# through CocoaPods at `pod install` time; our Bazel build has no CocoaPods
# integration, so it never appears. `@mdk_sdk_apple//:mdk_macos` (see
# MODULE.bazel, //third_party:mdk_sdk_apple.BUILD.bazel) vendors the same
# `mdk-sdk-apple.tar.xz` archive fvp's own podspec points at, pinned by
# sha256 for reproducibility (fvp's podspec itself has no such pin — it
# always resolves "nightly").
#
# Otherwise identical to the auto-generated spoke (see
# `flutter_pub_package`'s `_make_flutter_plugin_build_content`) — only the
# macOS `flutter_apple_plugin_library`'s `deps` differ.
#
# Substitutions ({HUB_NAME}, {PKG}, {VERSION}) are injected by
# `flutter_pub_package`'s `_resolve_overlay`. We don't read {VERSION} here —
# the overlay sits under `0/`, so any 0.x version routes here.

load("@bazel_skylib//rules:write_file.bzl", "write_file")
load("@rules_apple//apple:macos.bzl", "macos_framework")
load("@rules_cc//cc:objc_library.bzl", "objc_library")
load("@rules_flutter//flutter:defs.bzl", "flutter_plugin")
load("@rules_flutter//flutter:linux.bzl", "flutter_linux_plugin_library")
load("@rules_flutter//flutter:macos.bzl", "MACOS_MINIMUM_OS_VERSION", "flutter_apple_plugin_library")
load("@rules_flutter//flutter:windows.bzl", "flutter_windows_plugin_library")

flutter_plugin(
    name = "fvp",
    srcs = glob(["lib/**/*.dart"], allow_empty = True),
    deps = [
        "@{HUB_NAME}__ffi//:ffi",
        "@{HUB_NAME}__flutter//:flutter",
        "@{HUB_NAME}__http//:http",
        "@{HUB_NAME}__logging//:logging",
        "@{HUB_NAME}__path//:path",
        "@{HUB_NAME}__path_provider//:path_provider",
        "@{HUB_NAME}__plugin_platform_interface//:plugin_platform_interface",
        "@{HUB_NAME}__video_player//:video_player",
        "@{HUB_NAME}__video_player_platform_interface//:video_player_platform_interface",
    ],
    package_name = "fvp",
    plugin_platforms_json = "{\"android\":{\"package\":\"com.mediadevkit.fvp\",\"pluginClass\":\"FvpPlugin\"},\"elinux\":{\"dartPluginClass\":\"VideoPlayerRegistrant\",\"pluginClass\":\"FvpPlugin\"},\"ios\":{\"pluginClass\":\"FvpPlugin\",\"sharedDarwinSource\":true},\"linux\":{\"dartPluginClass\":\"VideoPlayerRegistrant\",\"pluginClass\":\"FvpPlugin\"},\"macos\":{\"pluginClass\":\"FvpPlugin\",\"sharedDarwinSource\":true},\"ohos\":{\"dartPluginClass\":\"VideoPlayerRegistrant\",\"pluginClass\":\"FvpPlugin\"},\"windows\":{\"dartPluginClass\":\"VideoPlayerRegistrant\",\"pluginClass\":\"FvpPluginCApi\"}}",
    language_version = "3.4",
    apple_libs = select({
        "@platforms//os:macos": [
            ":fvp_apple_macos",
        ],
        "@platforms//os:ios": [
            ":fvp_apple_ios",
        ],
        "//conditions:default": [],
    }),
    linux_libs = [":fvp_linux"],
    windows_libs = [":fvp_windows"],
    visibility = ["//visibility:public"],
)

flutter_apple_plugin_library(
    name = "fvp_apple_macos",
    srcs = glob(
        [
            "darwin/fvp/Sources/fvp/**/*.swift",
            "darwin/fvp/Sources/fvp/**/*.m",
            "darwin/fvp/Sources/fvp/**/*.mm",
            "darwin/fvp/Sources/fvp/**/*.h",
        ],
        exclude = [
            "darwin/fvp/Sources/fvp/test/**",
            "darwin/fvp/Sources/fvp/example/**",
        ],
        allow_empty = True,
    ),
    includes = [],
    module_name = "fvp",
    platform = "macos",
    # Root-module `http_archive(...)` (via `use_repo_rule`) calls get an
    # implicit-extension canonical name of the form `+http_archive+<name>` —
    # this label must be fully-qualified (`@@...`) since deps__fvp's own
    # repo mapping (owned by rules_flutter) has no apparent name for it.
    deps = ["@@+http_archive+mdk_sdk_apple//:mdk_macos"],
    visibility = ["//visibility:public"],
)

# `fvp.framework` — the FFI half of the plugin, as a dynamic framework.
#
# fvp splits its native code in two: `FvpPlugin.mm` (the platform-channel /
# texture side, built above and linked statically into the app executable
# like every other plugin) and `callbacks.cpp` (the C API Dart reaches over
# FFI). The FFI half cannot be statically linked: `package:fvp` loads it with
# `DynamicLibrary.open('fvp.framework/fvp')` (lib/src/lib.dart), the path
# CocoaPods' `use_frameworks!` build produces. Without a real framework at
# that path the app dies during video_player registration with
# "Failed to load dynamic library 'fvp.framework/fvp'".
#
# Splitting the two halves across a static lib and a dylib is safe here:
# on Apple platforms `callbacks.cpp` shares no state with `FvpPlugin.mm` and
# its single reference to it (`MdkGetPlayerVid`) is `#ifdef __ANDROID__`.
# Note that `flutter_apple_plugin_library` could not build this half anyway —
# its `_split_srcs` keeps only .swift/.m/.mm/.h and silently drops .cpp.
#
# `//frontend:kustavi_macos` embeds the result via `additional_contents`.
objc_library(
    name = "fvp_ffi_lib",
    srcs = ["darwin/fvp/Sources/fvp/callbacks.cpp"],
    hdrs = [
        "darwin/fvp/Sources/fvp/callbacks.h",
        "darwin/fvp/Sources/fvp/dart_api_types.h",
    ],
    # Nothing inside the framework references these symbols — Dart looks them
    # up with dlsym from outside — so without alwayslink the linker drops the
    # archive member and ships an empty dylib.
    alwayslink = True,
    # Mirrors `s.compiler_flags` in darwin/fvp.podspec.
    copts = [
        "-std=c++20",
        "-Wno-documentation",
    ],
    includes = ["darwin/fvp/Sources/fvp"],
    sdk_dylibs = ["c++"],
    target_compatible_with = ["@platforms//os:macos"],
    deps = ["@@+http_archive+mdk_sdk_apple//:mdk_macos"],
)

write_file(
    name = "fvp_framework_info_plist",
    out = "fvp_framework_Info.plist",
    content = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">',
        '<plist version="1.0">',
        "<dict>",
        "  <key>CFBundleName</key>",
        "  <string>fvp</string>",
        "  <key>CFBundlePackageType</key>",
        "  <string>FMWK</string>",
        "  <key>CFBundleShortVersionString</key>",
        "  <string>{VERSION}</string>",
        "  <key>CFBundleVersion</key>",
        "  <string>{VERSION}</string>",
        "</dict>",
        "</plist>",
    ],
)

# `macos_framework`, not `macos_dynamic_framework`: the latter fails unless it
# has exactly one swift_library dep, and this framework is pure ObjC++.
# rules_apple already links it with an `@executable_path/../Frameworks` rpath,
# which is how it finds its sibling `mdk.framework` in the app bundle.
macos_framework(
    name = "fvp_framework",
    bundle_id = "com.mediadevkit.fvp",
    bundle_name = "fvp",
    infoplists = [":fvp_framework_Info.plist"],
    minimum_os_version = MACOS_MINIMUM_OS_VERSION,
    target_compatible_with = ["@platforms//os:macos"],
    visibility = ["//visibility:public"],
    deps = [":fvp_ffi_lib"],
)

flutter_apple_plugin_library(
    name = "fvp_apple_ios",
    srcs = glob(
        [
            "darwin/fvp/Sources/fvp/**/*.swift",
            "darwin/fvp/Sources/fvp/**/*.m",
            "darwin/fvp/Sources/fvp/**/*.mm",
            "darwin/fvp/Sources/fvp/**/*.h",
        ],
        exclude = [
            "darwin/fvp/Sources/fvp/test/**",
            "darwin/fvp/Sources/fvp/example/**",
        ],
        allow_empty = True,
    ),
    includes = [],
    module_name = "fvp",
    platform = "ios",
    visibility = ["//visibility:public"],
)

flutter_linux_plugin_library(
    name = "fvp_linux",
    srcs = glob(
        [
            "linux/**/*.cc",
            "linux/**/*.cpp",
            "linux/**/*.c",
        ],
        exclude = [
            "linux/test/**",
            "linux/example/**",
        ],
        allow_empty = True,
    ),
    hdrs = glob(
        [
            "linux/**/*.h",
            "linux/**/*.hh",
            "linux/**/*.hpp",
        ],
        exclude = [
            "linux/test/**",
            "linux/example/**",
        ],
        allow_empty = True,
    ),
    includes = ["linux/include"],
    visibility = ["//visibility:public"],
)

flutter_windows_plugin_library(
    name = "fvp_windows",
    srcs = glob(
        [
            "windows/**/*.cc",
            "windows/**/*.cpp",
            "windows/**/*.c",
        ],
        exclude = [
            "windows/test/**",
            "windows/example/**",
        ],
        allow_empty = True,
    ),
    hdrs = glob(
        [
            "windows/**/*.h",
            "windows/**/*.hh",
            "windows/**/*.hpp",
        ],
        exclude = [
            "windows/test/**",
            "windows/example/**",
        ],
        allow_empty = True,
    ),
    includes = ["windows/include"],
    visibility = ["//visibility:public"],
)

exports_files(glob(
    ["**"],
    exclude = [
        "BUILD.bazel",
        "WORKSPACE",
        "WORKSPACE.bazel",
        "MODULE.bazel",
    ],
    allow_empty = True,
))
