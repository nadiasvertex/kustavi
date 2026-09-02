# Kustavi Windows installer (WiX v5)

`kustavi.wxs` builds a per-user `.msi` for the Kustavi desktop app.

## One-time setup

Install the .NET SDK, then the WiX v5 CLI as a global tool:

```
dotnet tool install --global wix
```

`wix` must be on `PATH` afterwards (a new shell usually picks it up).

## Building

```
just installer
```

which runs `python tools/package.py --installer`. That:

1. builds the release back end (`//backend:server --config=release`) and the
   Flutter Windows bundle (`//frontend:kustavi_windows`) with Bazel,
2. stages the full application tree into `dist/Kustavi/` (GUI, `kustavi-backend.exe`,
   llama.cpp DLLs, `cities.tsv`, `face_detection_yunet.onnx`, a `VERSION` marker),
3. runs `wix build` to produce `dist/Kustavi-<version>-x64.msi`.

`<version>` is the repo-root `VERSION` file, maintained by `tools/version.py`
(`just version-bump {major|minor|patch}`). The MSI `ProductVersion`, the payload
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

## Not handled on uninstall

The per-user data directory `%LOCALAPPDATA%\Kustavi` (downloaded vision-model
weights, per-folder session caches) is **left in place** on uninstall. Remove it
by hand to reclaim the disk space.
