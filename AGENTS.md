# Overview

  - Front end GUI: spec/frontend.md
  - Back end server: spec/backend.md
  - Protobuf RPC: spec/proto.md

Code conventions in `spec/CONVENTIONS` must be honored.
  - C++: spec/CONVENTIONS/C++.md

## Build

The repo builds with CMake (presets `debug`, `release`, `sanitize`; output in
`.build/<preset>`). Use the `just` recipes: `just build`, `just test-backend`,
`just test-quality`, `just test-video`, `just build-gui`, `just run-gui`,
`just test-gui` and `just package`. Dependency versions and checksums are in
`cmake/dependency_pins.cmake`.

The GUI (`gui/`) is C++ on Basalt widgets and Magma windows. It builds with
`-DKUSTAVI_BUILD_GUI=ON`, which also needs `-DKUSTAVI_BASALT_SOURCE_DIR=<checkout>` and
`-DKUSTAVI_MAGMA_SOURCE_DIR=<checkout>` until releases of the two are pinned (the `just`
recipes and `tools/package.py` read the same two names from the environment). Its layout is
in `spec/frontend.md` §14; the history of the rewrite is in `spec/plans/cpp-gui-rewrite.md`.

- `gui/core` holds the GUI's logic without any window or widget: domain types, the deletion
  model, the trips plan, the gRPC client, the back-end launcher and host, and the wizard
  controller. `common/process` holds the process launching that the GUI and the smoke client
  share. `just test-gui-core` runs their tests.
- `gui/app` holds the screens, the shell and `kustavi_gui`, the program.
  `kustavi_gui_preview --screen NAME --out FILE.png` draws a screen with a scripted back end
  (the names are in the header of `gui/app/preview.cpp`).
- Videos play through libmdk, a pinned release archive that the build fetches. Configure with
  `-DKUSTAVI_ENABLE_VIDEO=OFF` to build without it, or `-DKUSTAVI_MDK_DIR=<mdk-sdk>` to use
  another copy. Without libmdk a video shows its still image.
- Set `KUSTAVI_BACKEND_PATH` to run `kustavi_gui` against a back end that is not beside it
  (the build's is `backend/server`).
- The `sanitize` test preset leaves out the `grpc` label: those tests run a gRPC server, and
  the unsanitized gRPC and the sanitized test share inline library code, which the sanitizers
  report at server shutdown.

<!-- graft:start -->
## Graft — repo context graph

This repo is indexed in `graft/`: small linked markdown nodes that explain each
system and carry exact file:line spans, kept in sync with the code through git.

For ANY task here — understanding how something works, finding where code lives,
or scoping a change — get context from the graph before grepping or opening
source files. Re-ask freely (it's cheap) and reuse literal identifiers you
already have (symbol, error string, file name) as the query. New to this repo?
Run `graft map` first — a token-budgeted orientation (dir clusters, hubs,
hotspots), no LLM, no key.

- Run `graft ask "<your question>" --source` → ranked nodes with the relevant
  code spans inlined (each hit's ≤8-line crux by default; `--full` for whole
  definitions when the crux isn't enough). Match the tool to the task shape:
  for understanding or editing, the top node IS the answer — cite its
  `covers:` file:line spans and edit straight from `--source`. For
  exhaustive tasks ("every occurrence / every caller of this pattern"), ranked
  results are top-N, not complete — run `graft grep "<literal>"` instead
  (exhaustive over indexed files, grouped by enclosing symbol), falling back
  to raw `grep -rn` only for unindexed files.
- `graft skeleton <file>` → every definition's signature + span, ~10× cheaper
  than reading the file; use it to skim an API surface.
- `graft callers <symbol>` gives precomputed, exact edges — who calls this.
  Add `--direction out` for what it calls, or `--depth N` to walk
  transitively for the full blast radius. For structural questions, skip
  ranking and use this directly.
- Or browse: `graft/INDEX.md` lists every node; follow the links.
- Monorepos and folders of multiple repos rank fairly across sub-projects —
  hits carry `[scope/]` labels naming which one they're from. Narrow with
  `graft ask "<task>" --in <scope>/` once you know where you're working.

If a returned span is truncated ("+N more lines"), open the file at that exact
range before finalizing. Only open source files when a node genuinely lacks a
needed detail, and then at the exact file:line the node points to — never
re-read whole files.

After big code changes, refresh the graph with `graft build` (deterministic,
no API key, $0).
<!-- graft:end -->
