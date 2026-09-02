#!/usr/bin/env python3
"""Read and bump the single Kustavi version.

The repository-root ``VERSION`` file (``MAJOR.MINOR.PATCH``) is the one place a
human edits the app version. This script propagates it to every derived
location so the front end, the back end, the Bazel module, the Windows
executable metadata and the installer all agree:

  * ``MODULE.bazel``                        -- Bazel module version
  * ``frontend/pubspec.yaml``               -- Flutter bundle version (the
                                              ``+<build>`` suffix is preserved)
  * ``frontend/lib/src/version.dart``       -- ``kAppVersion`` runtime constant
  * ``backend/include/version.h``           -- ``kustavi::version`` (``--version``
                                              and the ``GetInfo`` RPC)
  * ``frontend/windows/runner/Runner.rc``   -- VERSIONINFO fallback literals

Usage:
    python tools/version.py show
    python tools/version.py set 1.4.0
    python tools/version.py bump patch          # 1.4.0 -> 1.4.1
    python tools/version.py bump minor          # 1.4.1 -> 1.5.0
    python tools/version.py bump major          # 1.5.0 -> 2.0.0
    python tools/version.py bump patch --tag    # also git commit + tag vX.Y.Z
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

# Repo root = parent of this file's directory (tools/).
REPO_ROOT = Path(__file__).resolve().parent.parent
VERSION_FILE = REPO_ROOT / "VERSION"

_SEMVER = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")


@dataclass(frozen=True)
class Target:
    """One derived location and the substitution that updates it."""

    path: Path
    pattern: re.Pattern[str]
    # Uses \g<1> / \g<2> back-references; {v} is filled with the new version.
    replacement: str


def _targets() -> list[Target]:
    return [
        # module(name = "kustavi", version = "X.Y.Z")
        Target(
            REPO_ROOT / "MODULE.bazel",
            re.compile(
                r'(module\(\s*\n\s*name\s*=\s*"kustavi",\s*\n\s*version\s*=\s*")'
                r"\d+\.\d+\.\d+"
                r'(")'
            ),
            r"\g<1>{v}\g<2>",
        ),
        # version: X.Y.Z+<build>   (keep the build suffix, whatever it is)
        Target(
            REPO_ROOT / "frontend" / "pubspec.yaml",
            re.compile(r"^(version:[ \t]*)\d+\.\d+\.\d+(\+\S+)?[ \t]*$", re.MULTILINE),
            r"\g<1>{v}\g<2>",
        ),
        # const String kAppVersion = 'X.Y.Z';
        Target(
            REPO_ROOT / "frontend" / "lib" / "src" / "version.dart",
            re.compile(r"(const String kAppVersion = ')[^']*(';)"),
            r"\g<1>{v}\g<2>",
        ),
        # inline constexpr std::string_view version = "X.Y.Z";
        Target(
            REPO_ROOT / "backend" / "include" / "version.h",
            re.compile(r'(inline constexpr std::string_view version = ")[^"]*(";)'),
            r"\g<1>{v}\g<2>",
        ),
        # #define VERSION_AS_NUMBER X,Y,Z,0
        Target(
            REPO_ROOT / "frontend" / "windows" / "runner" / "Runner.rc",
            re.compile(r"(#define VERSION_AS_NUMBER )\d+,\d+,\d+,\d+"),
            r"\g<1>{major},{minor},{patch},0",
        ),
        # #define VERSION_AS_STRING "X.Y.Z"
        Target(
            REPO_ROOT / "frontend" / "windows" / "runner" / "Runner.rc",
            re.compile(r'(#define VERSION_AS_STRING ")\d+\.\d+\.\d+(")'),
            r"\g<1>{v}\g<2>",
        ),
    ]


def read_version() -> str:
    """Return the current MAJOR.MINOR.PATCH from the VERSION file."""
    if not VERSION_FILE.exists():
        raise SystemExit(f"missing {VERSION_FILE}")
    text = VERSION_FILE.read_text(encoding="utf-8").strip()
    if not _SEMVER.match(text):
        raise SystemExit(f"{VERSION_FILE} is not MAJOR.MINOR.PATCH: {text!r}")
    return text


def _parse(version: str) -> tuple[int, int, int]:
    m = _SEMVER.match(version)
    if not m:
        raise SystemExit(f"not a MAJOR.MINOR.PATCH version: {version!r}")
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


def _bumped(current: str, component: str) -> str:
    major, minor, patch = _parse(current)
    if component == "major":
        return f"{major + 1}.0.0"
    if component == "minor":
        return f"{major}.{minor + 1}.0"
    return f"{major}.{minor}.{patch + 1}"


def propagate(new_version: str) -> list[Path]:
    """Rewrite every derived location to `new_version`. Returns changed files."""
    major, minor, patch = _parse(new_version)
    changed: list[Path] = []
    for target in _targets():
        if not target.path.exists():
            raise SystemExit(f"missing propagation target: {target.path}")
        original = target.path.read_text(encoding="utf-8")
        repl = target.replacement.format(
            v=new_version, major=major, minor=minor, patch=patch
        )
        updated, count = target.pattern.subn(repl, original)
        if count != 1:
            raise SystemExit(
                f"expected exactly one version match in {target.path} "
                f"(found {count}); the file drifted -- update tools/version.py"
            )
        if updated != original:
            target.path.write_text(updated, encoding="utf-8")
            changed.append(target.path)
    return changed


def _write_version_file(new_version: str) -> None:
    VERSION_FILE.write_text(f"{new_version}\n", encoding="utf-8")


def _git(*args: str) -> None:
    subprocess.run(["git", *args], cwd=REPO_ROOT, check=True)


def _apply(new_version: str, *, tag: bool) -> int:
    _parse(new_version)  # validate
    _write_version_file(new_version)
    changed = propagate(new_version)
    all_files = [VERSION_FILE, *changed]
    rels = sorted(
        {str(p.relative_to(REPO_ROOT)).replace("\\", "/") for p in all_files}
    )

    print(f"version -> {new_version}")
    for rel in rels:
        print(f"  updated {rel}")

    if tag:
        _git("add", *rels)
        _git("commit", "-m", f"Release v{new_version}")
        _git("tag", f"v{new_version}")
        print(f"committed and tagged v{new_version}")
    else:
        print()
        print("Next: review the diff, then commit, e.g.")
        print(f"  git add {' '.join(rels)}")
        print(f'  git commit -m "Release v{new_version}"')
        print(f"  git tag v{new_version}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)

    sub.add_parser("show", help="print the current version")

    p_set = sub.add_parser("set", help="set an explicit MAJOR.MINOR.PATCH version")
    p_set.add_argument("version")
    p_set.add_argument(
        "--tag", action="store_true",
        help="git add + commit the changes and create tag vX.Y.Z",
    )

    p_bump = sub.add_parser("bump", help="increment one version component")
    p_bump.add_argument("component", choices=("major", "minor", "patch"))
    p_bump.add_argument(
        "--tag", action="store_true",
        help="git add + commit the changes and create tag vX.Y.Z",
    )

    args = parser.parse_args(argv)

    if args.cmd == "show":
        print(read_version())
        return 0
    if args.cmd == "set":
        return _apply(args.version, tag=args.tag)
    if args.cmd == "bump":
        return _apply(_bumped(read_version(), args.component), tag=args.tag)
    parser.error(f"unknown command {args.cmd!r}")
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as exc:
        raise SystemExit(f"command failed ({exc.returncode}): {' '.join(map(str, exc.cmd))}")
