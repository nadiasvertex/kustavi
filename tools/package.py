#!/usr/bin/env python3
"""Build a redistributable Kustavi desktop package.

This is the implementation behind `just package`. It builds the release
back-end server and the Flutter desktop bundle with Bazel, assembles them
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
the GeoNames place table, the YuNet face model, and the llama.cpp shared
libraries are bundled alongside it (see backend/include/paths.h).

Usage:
    python tools/package.py                 # package for the host OS
    python tools/package.py --target macos  # explicit; must match the host
    python tools/package.py --keep          # leave the staging tree, skip zip
    python tools/package.py --skip-build    # reuse existing bazel-bin outputs
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

# llama.cpp shared libraries, by platform. The Bazel build drops these next to
# the server binary (Windows) or into its runfiles tree (macOS); either way the
# packaged build needs them beside kustavi-backend.
LLAMA_LIB_PATTERNS = {
    "macos": ("libllama", "libggml", "libmtmd"),
    "windows": ("llama.dll", "ggml", "mtmd.dll"),
}


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


def bazel_bin_for(label: str, config_args: list[str]) -> Path:
    """Resolve the `bin` directory that actually holds `label`'s outputs.

    A package run builds the back-end with `--config=release` and the GUI
    bundle without it, so the two land in different `bazel-out/<config>/bin`
    trees. The repo-root `bazel-bin` symlink only points at whichever build
    ran last, so ask Bazel where each target's files really are.
    """
    proc = subprocess.run(
        ["bazel", "cquery", label, "--output=files", *config_args],
        cwd=REPO_ROOT, check=True, capture_output=True, text=True,
    )
    files = proc.stdout.split()
    if not files:
        raise SystemExit(f"bazel cquery returned no files for {label}")
    first = (REPO_ROOT / files[0]).resolve()
    for parent in first.parents:
        if parent.name == "bin":
            return parent
    raise SystemExit(f"could not find a 'bin' directory above {first}")


def copy_file(src: Path, dst: Path, *, executable: bool = False) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    # Bazel outputs are read-only; make the copy writable so a re-run can
    # overwrite it, and mark binaries executable for the archive.
    mode = 0o755 if executable else 0o644
    os.chmod(dst, mode)


def find_llama_libs(target: str, search_roots: list[Path]) -> list[Path]:
    """Return one path per distinct llama.cpp shared-library basename."""
    prefixes = LLAMA_LIB_PATTERNS[target]
    suffix = ".dylib" if target == "macos" else ".dll"
    found: dict[str, Path] = {}
    for root in search_roots:
        if not root.exists():
            continue
        for dirpath, _dirnames, filenames in os.walk(root, followlinks=True):
            for name in filenames:
                if not name.endswith(suffix):
                    continue
                if not name.startswith(prefixes):
                    continue
                found.setdefault(name, Path(dirpath) / name)
    return sorted(found.values())


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
        run(["bazel", "build", "//backend:server", "--config=release"])
        run(["bazel", "build", "//frontend:kustavi_macos"])

    backend_bin = bazel_bin_for("//backend:server", ["--config=release"])
    gui_bin = bazel_bin_for("//frontend:kustavi_macos", [])

    print("[2/4] Unpacking the GUI bundle")
    gui_zip = gui_bin / "frontend" / "kustavi_macos.zip"
    if not gui_zip.exists():
        raise SystemExit(f"missing {gui_zip} -- run without --skip-build")
    app_dir = dist / f"{APP_NAME}.app"
    # Use ditto/unzip rather than zipfile so bundle symlinks and the +x bit on
    # the runner executable survive extraction.
    if shutil.which("ditto"):
        run(["ditto", "-x", "-k", str(gui_zip), str(dist)])
    elif shutil.which("unzip"):
        run(["unzip", "-q", str(gui_zip), "-d", str(dist)])
    else:
        with zipfile.ZipFile(gui_zip) as zf:
            zf.extractall(dist)
    macos_dir = app_dir / "Contents" / "MacOS"
    macos_dir.mkdir(parents=True, exist_ok=True)

    print("[3/4] Bundling the back-end, data files and llama.cpp libraries")
    copy_file(backend_bin / "backend" / "server", macos_dir / BACKEND_STEM,
              executable=True)
    for name in DATA_FILES:
        copy_file(REPO_ROOT / "backend" / "data" / name, macos_dir / name)

    libs = find_llama_libs("macos", [
        backend_bin / "backend" / "server.runfiles",
        backend_bin / "backend",
    ])
    if not libs:
        raise SystemExit("no llama.cpp dylibs found in the server build outputs")
    for lib in libs:
        copy_file(lib, macos_dir / lib.name, executable=True)

    # Version marker so an unpacked bundle is identifiable (matches the MSI
    # ProductVersion / archive name on Windows).
    version = read_version()
    (macos_dir / "VERSION").write_text(f"{version}\n", encoding="utf-8")

    # Bazel's build rpath points into the sandbox and does not survive the
    # copy; repoint the binary at its own directory.
    if shutil.which("install_name_tool"):
        try:
            run(["install_name_tool", "-add_rpath", "@loader_path",
                 str(macos_dir / BACKEND_STEM)])
        except subprocess.CalledProcessError:
            print("  ! install_name_tool failed (rpath may already be set)")

    if args.keep:
        print("[4/4] --keep: leaving dist/ unpacked, skipping the archive")
        return app_dir

    print("[4/4] Writing the redistributable archive")
    archive = dist / f"kustavi-macos-{version}.zip"
    make_macos_zip(dist, archive)
    return archive


# --------------------------------------------------------------------------- #
# Windows
# --------------------------------------------------------------------------- #
def package_windows(args: argparse.Namespace) -> Path:
    dist = REPO_ROOT / "dist"

    if not args.skip_build:
        print("[1/4] Building release back-end + Windows GUI bundle")
        run(["bazel", "build", "//backend:server", "--config=release"])
        run(["bazel", "build", "//frontend:kustavi_windows"])

    backend_bin = bazel_bin_for("//backend:server", ["--config=release"])
    gui_bin = bazel_bin_for("//frontend:kustavi_windows", [])

    print("[2/4] Staging the GUI bundle")
    # flutter_windows_app emits a tree artifact named after the target.
    gui_bundle = gui_bin / "frontend" / "kustavi_windows"
    if not gui_bundle.is_dir():
        raise SystemExit(f"missing {gui_bundle} -- run without --skip-build")
    app_dir = dist / APP_NAME
    if app_dir.exists():
        shutil.rmtree(app_dir)
    shutil.copytree(gui_bundle, app_dir)
    # Give the runner a friendly name (mirrors app_name = "Kustavi" on macOS).
    runner = app_dir / "kustavi_windows.exe"
    if runner.exists():
        runner.rename(app_dir / f"{APP_NAME}.exe")
    for path in app_dir.rglob("*"):
        if path.is_file():
            os.chmod(path, 0o755 if path.suffix in (".exe", ".dll") else 0o644)

    print("[3/4] Bundling the back-end, data files and llama.cpp libraries")
    copy_file(backend_bin / "backend" / "server.exe",
              app_dir / f"{BACKEND_STEM}.exe", executable=True)
    for name in DATA_FILES:
        copy_file(REPO_ROOT / "backend" / "data" / name, app_dir / name)

    libs = find_llama_libs("windows", [
        backend_bin / "backend",
        backend_bin / "backend" / "server.exe.runfiles",
    ])
    if not libs:
        raise SystemExit("no llama.cpp DLLs found in the server build outputs")
    for lib in libs:
        copy_file(lib, app_dir / lib.name, executable=True)

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
    by tools/installer/kustavi.wxs.
    """
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
        help="reuse existing bazel-bin outputs instead of building",
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
