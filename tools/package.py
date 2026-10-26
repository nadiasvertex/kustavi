#!/usr/bin/env python3
"""Build a redistributable Kustavi desktop package.

This is the implementation behind `just package`. It builds the release
C++ GUI, which holds the back end as a library, with CMake, assembles it into a
self-contained application directory under `dist/`, and zips that directory into
a redistributable archive.

A build is always produced for the host OS -- the toolchains that assemble a
macOS `.app` or a Windows runner only run natively -- so `--target` may only
name the current platform.

Layout produced under `dist/`:

    macOS      dist/Kustavi.app/                  (unpacked, used by `just run`)
               dist/Kustavi.app/Contents/MacOS/Kustavi
               dist/Kustavi.app/Contents/Resources/fonts
               dist/Kustavi.app/Contents/Frameworks/mdk.framework
               dist/kustavi-macos-<version>.zip   (Kustavi.app/...)

    Windows    dist/Kustavi/                      (unpacked)
               dist/Kustavi/Kustavi.exe
               dist/Kustavi/fonts
               dist/Kustavi/mdk.dll and the libraries that it loads
               dist/Kustavi/VERSION               (payload version marker)
               dist/kustavi-windows-<version>.zip (Kustavi/...)
               dist/Kustavi-<version>-x64.msi     (with --installer; WiX v5)

The version is the repo-root VERSION.txt file, maintained by tools/version.py.

The GeoNames place table and the YuNet face model sit beside the GUI executable (see
backend/include/paths.h), and the bundled fonts in `fonts` (see gui/app/fonts.h). llama.cpp is
linked statically into the GUI, and so are Basalt and Magma; only libmdk, which plays videos,
ships as a separate library.

The build uses the Basalt and Magma pinned in cmake/dependency_pins.cmake. To use checkouts
instead, pass --basalt-dir and --magma-dir, or set KUSTAVI_BASALT_SOURCE_DIR and
KUSTAVI_MAGMA_SOURCE_DIR.

Usage:
    python tools/package.py                 # package for the host OS
    python tools/package.py --target macos  # explicit; must match the host
    python tools/package.py --keep          # leave the staging tree, skip zip
    python tools/package.py --skip-build    # reuse existing build outputs
    python tools/package.py --installer     # Windows: build the WiX MSI
    python tools/package.py --basalt-dir ~/basalt --magma-dir ~/magma
"""

from __future__ import annotations

import argparse
import os
import platform
import plistlib
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

# Repo root = parent of this file's directory (tools/).
REPO_ROOT = Path(__file__).resolve().parent.parent

APP_NAME = "Kustavi"

# WiX v5 installer sources (Windows only). See tools/installer/README.md.
WXS_FILE = REPO_ROOT / "tools" / "installer" / "kustavi.wxs"


def read_version() -> str:
    """Return the app version from the repo-root VERSION.txt file (MAJOR.MINOR.PATCH).

    This is the single source of truth maintained by tools/version.py; the
    packaged archive, the payload VERSION marker and the MSI ProductVersion all
    derive from it so a build is always identifiable. (The file is VERSION.txt,
    not VERSION, so it does not shadow the C++ <version> header on Windows.)
    """
    text = (REPO_ROOT / "VERSION.txt").read_text(encoding="utf-8").strip()
    if not text:
        raise SystemExit("VERSION.txt file is empty")
    return text

# Small data files bundled next to the program. paths.h looks for these
# beside the executable first in a packaged build.
DATA_FILES = ("cities.tsv", "face_detection_yunet.onnx")

def host_target() -> str:
    system = platform.system()
    if system == "Darwin":
        return "macos"
    if system == "Windows":
        return "windows"
    raise SystemExit(f"Unsupported host OS for packaging: {system!r}")


def run(cmd: list[str]) -> None:
    print(f"  $ {' '.join(cmd)}", flush=True)
    subprocess.run(cmd, cwd=REPO_ROOT, check=True)


def build_release(args: argparse.Namespace) -> None:
    """Build the release GUI with CMake."""
    configure = ["cmake", "--preset", "release", "-DKUSTAVI_BUILD_GUI=ON"]
    for name, value in (("KUSTAVI_BASALT_SOURCE_DIR", args.basalt_dir),
                        ("KUSTAVI_MAGMA_SOURCE_DIR", args.magma_dir)):
        if value:
            configure.append(f"-D{name}={Path(value).expanduser().resolve()}")
    run(configure)
    run(["cmake", "--build", "--preset", "release", "--target", "kustavi_gui"])


def build_dir() -> Path:
    return REPO_ROOT / ".build" / "release"


def mdk_dir() -> Path:
    """The mdk-sdk that the release build linked, or none when it has no video."""
    return build_dir() / "_deps" / "mdk-src"


def copy_file(src: Path, dst: Path, *, executable: bool = False) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    # Make the copy writable so a re-run can overwrite it, and mark binaries
    # executable for the archive.
    mode = 0o755 if executable else 0o644
    os.chmod(dst, mode)


def make_zip(staging: Path, top_level: str, archive: Path) -> None:
    """Zip `staging/top_level` into `archive`, preserving the top-level dir."""
    if archive.exists():
        archive.unlink()
    root = staging / top_level
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zf:
        for path in sorted(root.rglob("*")):
            arcname = path.relative_to(staging)
            if path.is_symlink():
                # Store the symlink itself, not its target.
                zi = zipfile.ZipInfo(str(arcname))
                zi.create_system = 3  # Unix
                zi.external_attr = (0xA1FF << 16)  # symlink, 0777
                zf.writestr(zi, os.readlink(path))
            elif path.is_dir():
                continue
            else:
                zi = zipfile.ZipInfo.from_file(path, str(arcname))
                zi.compress_type = zipfile.ZIP_DEFLATED
                # Preserve the executable bit so unpacked binaries stay runnable.
                perm = 0o755 if os.access(path, os.X_OK) else 0o644
                zi.external_attr = perm << 16
                with open(path, "rb") as fh:
                    zf.writestr(zi, fh.read())


def make_macos_zip(staging: Path, archive: Path) -> None:
    """Prefer `ditto` on macOS: it preserves symlinks, perms and bundle bits."""
    if archive.exists():
        archive.unlink()
    if shutil.which("ditto"):
        run([
            "ditto", "-c", "-k", "--sequesterRsrc", "--keepParent",
            str(staging / f"{APP_NAME}.app"), str(archive),
        ])
    else:
        make_zip(staging, f"{APP_NAME}.app", archive)


# --------------------------------------------------------------------------- #
# macOS
# --------------------------------------------------------------------------- #
# The Flutter app used com.example.frontend. macOS ties the folder access that a user granted to the
# bundle id, so a user of that app is asked for access again.
BUNDLE_ID = "io.github.nadiasvertex.kustavi"
COPYRIGHT = "Copyright \u00a9 2026 Kustavi. All rights reserved."


def minimum_macos_version(binary: Path) -> str:
    """The oldest macOS the binary runs on, read from its load commands."""
    text = subprocess.run(["otool", "-l", str(binary)], check=True, capture_output=True,
                          text=True).stdout
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if line.strip() == "cmd LC_BUILD_VERSION":
            for follow in lines[index:index + 8]:
                if follow.strip().startswith("minos"):
                    return follow.split()[1]
    return "11.0"


def make_icns(png: Path, icns: Path) -> None:
    """Make an .icns from a square PNG with sips and iconutil, which ship with macOS."""
    iconset = icns.with_suffix(".iconset")
    if iconset.exists():
        shutil.rmtree(iconset)
    iconset.mkdir(parents=True)
    for size in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            pixels = size * scale
            name = f"icon_{size}x{size}{'@2x' if scale == 2 else ''}.png"
            run(["sips", "-z", str(pixels), str(pixels), str(png), "--out",
                 str(iconset / name)])
    run(["iconutil", "-c", "icns", str(iconset), "-o", str(icns)])
    shutil.rmtree(iconset)


def write_info_plist(contents: Path, version: str, minimum: str) -> None:
    plist = {
        "CFBundleDevelopmentRegion": "en",
        "CFBundleExecutable": APP_NAME,
        "CFBundleIconFile": APP_NAME,
        "CFBundleIdentifier": BUNDLE_ID,
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": APP_NAME,
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": version,
        "CFBundleVersion": version,
        "LSMinimumSystemVersion": minimum,
        "NSHighResolutionCapable": True,
        "NSHumanReadableCopyright": COPYRIGHT,
        "NSPrincipalClass": "NSApplication",
    }
    with open(contents / "Info.plist", "wb") as handle:
        plistlib.dump(plist, handle)


def bundle_mdk(app_dir: Path, executable: Path) -> None:
    """Copy libmdk into the bundle and make the program look for it there."""
    framework = mdk_dir() / "lib" / "mdk.framework"
    if not framework.is_dir():
        framework = next((mdk_dir() / "lib" / "mdk.xcframework").glob("macos-*/mdk.framework"),
                         None)
    if framework is None or not framework.is_dir():
        print("  (no libmdk in the build: videos show their still image)")
        return
    frameworks = app_dir / "Contents" / "Frameworks"
    frameworks.mkdir(parents=True, exist_ok=True)
    shutil.copytree(framework, frameworks / "mdk.framework", symlinks=True)
    # The build finds the framework by the path in the build tree, which does not exist on
    # another machine.
    otool = subprocess.run(["otool", "-l", str(executable)], check=True, capture_output=True,
                           text=True).stdout
    for line in otool.splitlines():
        line = line.strip()
        if line.startswith("path ") and "mdk" in line and "(offset" in line:
            run(["install_name_tool", "-delete_rpath", line.split()[1], str(executable)])
    run(["install_name_tool", "-add_rpath", "@executable_path/../Frameworks", str(executable)])


def copy_fonts(destination: Path) -> None:
    """Copies the bundled UI and icon fonts, with their licenses."""
    source = REPO_ROOT / "resources" / "fonts"
    for path in sorted(source.iterdir()):
        if path.suffix in (".ttf", ".txt"):
            copy_file(path, destination / path.name)


def package_macos(args: argparse.Namespace) -> Path:
    dist = REPO_ROOT / "dist"

    if not args.skip_build:
        print("[1/4] Building the release back end and GUI")
        build_release(args)

    print("[2/4] Assembling the application bundle")
    gui = build_dir() / "bin" / "kustavi_gui"
    if not gui.is_file():
        raise SystemExit(f"missing {gui} -- run without --skip-build")
    version = read_version()
    app_dir = dist / f"{APP_NAME}.app"
    contents = app_dir / "Contents"
    macos_dir = contents / "MacOS"
    (contents / "Resources").mkdir(parents=True)
    copy_file(gui, macos_dir / APP_NAME, executable=True)
    make_icns(REPO_ROOT / "resources" / "icon-square.png",
              contents / "Resources" / f"{APP_NAME}.icns")
    copy_fonts(contents / "Resources" / "fonts")
    write_info_plist(contents, version, minimum_macos_version(gui))
    bundle_mdk(app_dir, macos_dir / APP_NAME)

    print("[3/4] Bundling the data files")
    for name in DATA_FILES:
        copy_file(REPO_ROOT / "backend" / "data" / name, macos_dir / name)

    # Version marker so an unpacked bundle is identifiable (matches the MSI
    # ProductVersion / archive name on Windows).
    (macos_dir / "VERSION").write_text(f"{version}\n", encoding="utf-8")

    strip_mdk_local_rpaths(app_dir)
    # A bundle that was changed after it was built needs a new signature to run. This one is
    # ad hoc, which is enough for a machine that built it or a user who allows it.
    if shutil.which("codesign"):
        run(["codesign", "--force", "--deep", "--sign", "-", str(app_dir)])

    if args.keep:
        print("[4/4] --keep: leaving dist/ unpacked, skipping the archive")
        return app_dir

    print("[4/4] Writing the redistributable archive")
    archive = dist / f"kustavi-macos-{version}.zip"
    make_macos_zip(dist, archive)
    return archive


# rpaths the prebuilt mdk framework carries for developer machines. mdk
# weak-links libavdevice, which its own bundled libffmpeg.9.dylib does not
# provide, so dyld follows these and loads whatever ffmpeg Homebrew/MacPorts
# happens to have installed -- yielding "Class AVFFrameReceiver is implemented
# in both ..." at launch and an app whose media stack differs per machine.
# Nothing we ship needs libavdevice (it is capture-device input; playback comes
# out of the bundled libffmpeg), so drop the paths.
MDK_LOCAL_RPATHS = ("/opt/homebrew/lib", "/usr/local/lib")


def strip_mdk_local_rpaths(app_dir: Path) -> None:
    """Remove /opt/homebrew/lib and /usr/local/lib from the bundled mdk."""
    mdk = app_dir / "Contents/Frameworks/mdk.framework/Versions/A/mdk"
    if not mdk.exists() or not shutil.which("install_name_tool"):
        return
    existing = subprocess.run(["otool", "-l", str(mdk)], check=True,
                              capture_output=True, text=True).stdout
    for rpath in MDK_LOCAL_RPATHS:
        if f"path {rpath} (offset" in existing:
            run(["install_name_tool", "-delete_rpath", rpath, str(mdk)])


# --------------------------------------------------------------------------- #
# Windows
# --------------------------------------------------------------------------- #
def package_windows(args: argparse.Namespace) -> Path:
    dist = REPO_ROOT / "dist"

    if not args.skip_build:
        print("[1/4] Building the release back end and GUI")
        build_release(args)

    print("[2/4] Staging the GUI")
    gui = build_dir() / "bin" / "kustavi_gui.exe"
    if not gui.is_file():
        raise SystemExit(f"missing {gui} -- run without --skip-build")
    app_dir = dist / APP_NAME
    if app_dir.exists():
        shutil.rmtree(app_dir)
    copy_file(gui, app_dir / f"{APP_NAME}.exe", executable=True)
    copy_fonts(app_dir / "fonts")
    # libmdk and the libraries that it loads sit beside the program.
    mdk_bin = mdk_dir() / "bin" / "x64"
    if mdk_bin.is_dir():
        for path in sorted(mdk_bin.glob("*.dll")):
            copy_file(path, app_dir / path.name, executable=True)
    else:
        print("  (no libmdk in the build: videos show their still image)")

    print("[3/4] Bundling the data files")
    for name in DATA_FILES:
        copy_file(REPO_ROOT / "backend" / "data" / name, app_dir / name)

    # Version marker inside the payload so both the installed tree and the zip
    # carry the version that produced them.
    version = read_version()
    (app_dir / "VERSION").write_text(f"{version}\n", encoding="utf-8")

    if args.installer:
        print(f"[4/4] Building the WiX MSI installer (v{version})")
        return build_msi(dist, app_dir, version)

    if args.keep:
        print("[4/4] --keep: leaving dist/ unpacked, skipping the archive")
        return app_dir

    print("[4/4] Writing the redistributable archive")
    archive = dist / f"kustavi-windows-{version}.zip"
    make_zip(dist, APP_NAME, archive)
    return archive


def build_msi(dist: Path, staging: Path, version: str) -> Path:
    """Run `wix build` over the staged dist/Kustavi tree -> dist/Kustavi-<v>-x64.msi.

    Requires WiX v5 on PATH (`dotnet tool install --global wix`). The whole
    staging tree is harvested automatically via the `Stage` bindpath referenced
    by tools/installer/kustavi.wxs; the `Resources` bindpath points at the
    resources directory so the MSI's ARP / shortcut icon is the app icon
    (app_icon.ico, generated from icon.png) rather than the default Windows
    Installer icon.
    """
    icon_dir = REPO_ROOT / "resources"
    if shutil.which("wix") is None:
        raise SystemExit(
            "`wix` not found on PATH. Install WiX v5 with:\n"
            "    dotnet tool install --global wix"
        )
    if not WXS_FILE.exists():
        raise SystemExit(f"missing installer source: {WXS_FILE}")
    msi = dist / f"{APP_NAME}-{version}-x64.msi"
    if msi.exists():
        msi.unlink()
    run([
        "wix", "build", str(WXS_FILE),
        "-arch", "x64",
        "-d", f"Version={version}",
        "-bindpath", f"Stage={staging}",
        "-bindpath", f"Resources={icon_dir}",
        "-out", str(msi),
    ])
    return msi


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--target", choices=("host", "macos", "windows"), default="host",
        help="platform to package for (default: the host OS)",
    )
    parser.add_argument(
        "--keep", action="store_true",
        help="leave the unpacked staging tree in dist/ and skip zipping",
    )
    parser.add_argument(
        "--skip-build", action="store_true",
        help="reuse existing build outputs instead of building",
    )
    parser.add_argument(
        "--installer", action="store_true",
        help="build the WiX MSI installer instead of the zip (Windows only; "
             "needs `dotnet tool install --global wix`)",
    )
    parser.add_argument(
        "--basalt-dir", default=os.environ.get("KUSTAVI_BASALT_SOURCE_DIR"),
        help="a Basalt checkout to build the GUI against (default: $KUSTAVI_BASALT_SOURCE_DIR)",
    )
    parser.add_argument(
        "--magma-dir", default=os.environ.get("KUSTAVI_MAGMA_SOURCE_DIR"),
        help="a Magma checkout to build the GUI against (default: $KUSTAVI_MAGMA_SOURCE_DIR)",
    )
    args = parser.parse_args()

    target = host_target() if args.target == "host" else args.target
    host = host_target()
    if target != host:
        raise SystemExit(
            f"cannot package for {target!r} on a {host!r} host: the GUI bundle "
            f"toolchain only runs natively."
        )
    if args.installer and target != "windows":
        raise SystemExit("--installer is only supported on Windows")

    dist = REPO_ROOT / "dist"
    print(f"Packaging Kustavi for {target} (repo: {REPO_ROOT})")
    if dist.exists():
        shutil.rmtree(dist)
    dist.mkdir(parents=True)

    result = (package_macos if target == "macos" else package_windows)(args)

    size_mb = ""
    if result.is_file():
        size_mb = f" ({result.stat().st_size / 1e6:.1f} MB)"
    print(f"\nDone (v{read_version()}): {result}{size_mb}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as exc:
        raise SystemExit(f"command failed ({exc.returncode}): {' '.join(exc.cmd)}")
