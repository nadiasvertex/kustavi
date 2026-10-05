# Kustavi Windows installer (WiX v5)

`kustavi.wxs` builds a per-user `.msi` for the Kustavi desktop app.

## One-time setup

Install the .NET SDK, then the WiX v5 CLI as a global tool:

```
dotnet tool install --global wix
```

`wix` must be on `PATH` afterwards (a new shell usually picks it up). The
standard installer's wizard also needs the WiX UI extension:

```
wix extension add --global WixToolset.UI.wixext/5.0.2
```

## Building

```
just installer
```

which runs `python tools/package.py --installer`. That:

1. builds the release GUI, which holds the back end (the `release` CMake preset, the
   `kustavi_gui` target),
2. stages the full application tree into `dist/Kustavi/` (`Kustavi.exe`, libmdk and its
   libraries, `fonts`, `cities.tsv`, `face_detection_yunet.onnx`, a `VERSION`
   marker),
3. runs `wix build` twice over the same `kustavi.wxs` to produce two installers:
   - `dist/Kustavi-<version>-x64.msi` — the standard installer: a wizard
     (welcome, install-folder choice, progress, finish).
   - `dist/Kustavi-<version>-x64-store.msi` — for the Microsoft Store: no UI,
     no questions, it just installs. Selected by `-d Store=1`.

`<version>` is the repo-root `VERSION.txt` file, maintained by `tools/version.py`
(`just bump-version {major|minor|patch}`). The MSI `ProductVersion`, the payload
`VERSION` marker and the file name all come from that one value.

To rebuild the MSI without rebuilding the app:

```
python tools/package.py --installer --skip-build
```

## Behaviour

- **Per-user**, installs to `%LOCALAPPDATA%\Programs\Kustavi` — no UAC / admin
  prompt.
- Adds a **Start Menu** shortcut ("Kustavi").
- `MajorUpgrade` — running a newer MSI over an existing install replaces it in
  place; a downgrade is refused with a message.
- Listed in **Settings → Apps → Installed apps** as "Kustavi <version>",
  publisher "Kustavi".
- ARP entry and Start Menu shortcut show the app icon
  (`resources/app_icon.ico`, generated from `resources/icon.png`), passed to `wix build` via the `Resources` bindpath.

## Not handled on uninstall

The per-user data directory `%LOCALAPPDATA%\Kustavi` (downloaded vision-model
weights, per-folder session caches) is **left in place** on uninstall. Remove it
by hand to reclaim the disk space.
