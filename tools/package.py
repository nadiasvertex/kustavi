#!/usr/bin/env python3
"""Build a redistributable Kustavi desktop package.

This is the implementation behind `just package`. It builds the release
back-end server with CMake and the Flutter desktop bundle with `flutter build`,
assembles them
into a single self-contained application directory under `dist/`, and zips
that directory into a redistributable archive.

A build is always produced for the host OS -- the toolchains that assemble a
macOS `.app` or a Windows runner only run natively -- so `--target` may only
name the current platform.

Layout produced under `dist/`:

    macOS      dist/Kustavi.app/                  (unpacked, used by `just run`)
               dist/Contents/MacOS/kustavi-backend
               dist/kustavi-macos-<version>.zip   (Kustavi.app/...)

    Windows    dist/Kustavi/                      (unpacked)
               dist/Kustavi/Kustavi.exe
               dist/Kustavi/kustavi-backend.exe
               dist/Kustavi/VERSION               (payload version marker)
               dist/kustavi-windows-<version>.zip (Kustavi/...)
               dist/Kustavi-<version>-x64.msi     (with --installer; WiX v5)

The version is the repo-root VERSION.txt file, maintained by tools/version.py.

The back-end binary is placed next to the GUI executable so the front end
finds it (see frontend/lib/src/backend/process.dart::findBackendBinary), and
the GeoNames place table and the YuNet face model are bundled alongside it
(see backend/include/paths.h). llama.cpp is linked statically into the server.

Usage:
    python tools/package.py                 # package for the host OS
    python tools/package.py --target macos  # explicit; must match the host
    python tools/package.py --keep          # leave the staging tree, skip zip
    python tools/package.py --skip-build    # reuse existing build outputs
    python tools/package.py --installer     # Windows: build the WiX MSI
"""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

# Repo root = parent of this file's directory (tools/).
REPO_ROOT = Path(__file__).resolve().parent.parent

APP_NAME = "Kustavi"
BACKEND_STEM = "kustavi-backend"

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

# Small data files bundled next to the back-end binary. paths.h looks for these
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


def build_backend() -> Path:
    """Build the release server with CMake and return its output directory."""
    run(["cmake", "--preset", "release"])
    run(["cmake", "--build", "--preset", "release", "--target", "server"])
    return REPO_ROOT / ".build" / "release" / "backend"


def backend_dir() -> Path:
    return REPO_ROOT / ".build" / "release" / "backend"


def build_flutter(target: str) -> None:
    """Build the release Flutter desktop bundle with the host Flutter SDK."""
    subprocess.run(["flutter", "build", target, "--release"],
                   cwd=REPO_ROOT / "frontend", check=True)


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
def package_macos(args: argparse.Namespace) -> Path:
    dist = REPO_ROOT / "dist"

    if not args.skip_build:
        print("[1/4] Building release back-end + macOS GUI bundle")
        build_backend()
        build_flutter("macos")

    print("[2/4] Copying the GUI bundle")
    gui_app = (REPO_ROOT / "frontend" / "build" / "macos" / "Build" / "Products"
               / "Release" / f"{APP_NAME}.app")
    if not gui_app.is_dir():
        raise SystemExit(f"missing {gui_app} -- run without --skip-build")
    app_dir = dist / f"{APP_NAME}.app"
    # symlinks=True keeps the framework symlinks inside the bundle intact.
    shutil.copytree(gui_app, app_dir, symlinks=True)
    macos_dir = app_dir / "Contents" / "MacOS"

    print("[3/4] Bundling the back-end and data files")
    copy_file(backend_dir() / "server", macos_dir / BACKEND_STEM,
              executable=True)
    for name in DATA_FILES:
        copy_file(REPO_ROOT / "backend" / "data" / name, macos_dir / name)

    # Version marker so an unpacked bundle is identifiable (matches the MSI
    # ProductVersion / archive name on Windows).
    version = read_version()
    (macos_dir / "VERSION").write_text(f"{version}\n", encoding="utf-8")

    strip_mdk_local_rpaths(app_dir)

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
    removed = False
    for rpath in MDK_LOCAL_RPATHS:
        if f"path {rpath} (offset" not in existing:
            continue
        run(["install_name_tool", "-delete_rpath", rpath, str(mdk)])
        removed = True
    if not removed:
        return
    # install_name_tool invalidates the framework's signature; re-sign ad hoc so dyld still loads it.
    if shutil.which("codesign"):
        run(["codesign", "--force", "--sign", "-",
             str(app_dir / "Contents/Frameworks/mdk.framework")])


# --------------------------------------------------------------------------- #
# Windows
# --------------------------------------------------------------------------- #
def package_windows(args: argparse.Namespace) -> Path:
    dist = REPO_ROOT / "dist"

    if not args.skip_build:
        print("[1/4] Building release back-end + Windows GUI bundle")
        build_backend()
        build_flutter("windows")

    print("[2/4] Staging the GUI bundle")
    gui_bundle = (REPO_ROOT / "frontend" / "build" / "windows" / "x64"
                  / "runner" / "Release")
    if not gui_bundle.is_dir():
        raise SystemExit(f"missing {gui_bundle} -- run without --skip-build")
    app_dir = dist / APP_NAME
    if app_dir.exists():
        shutil.rmtree(app_dir)
    shutil.copytree(gui_bundle, app_dir)
    for path in app_dir.rglob("*"):
        if path.is_file():
            os.chmod(path, 0o755 if path.suffix in (".exe", ".dll") else 0o644)

    print("[3/4] Bundling the back-end and data files")
    copy_file(backend_dir() / "server.exe",
              app_dir / f"{BACKEND_STEM}.exe", executable=True)
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
    Flutter Windows runner resources so the MSI's ARP / shortcut icon is the
    app icon (app_icon.ico, generated from resources/icon.png) rather than the
    default Windows Installer icon.
    """
    icon_dir = REPO_ROOT / "frontend" / "windows" / "runner" / "resources"
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
