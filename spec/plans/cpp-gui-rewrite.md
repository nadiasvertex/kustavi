# Plan: Rewrite the GUI in C++ with Basalt and Magma

This plan replaces the Flutter front end with a C++ desktop application built on Basalt (widget
tree, layout, text, standard widgets) and Magma (windows, input, frame pacing, rendering). The back
end, the gRPC protocol, and the user-visible behavior in `spec/frontend.md` stay the same; the
spec's screens and transition table are the acceptance criteria. The repo moves from Bazel to
CMake before the GUI work starts, because the main reason for Bazel was the Flutter build, and
Basalt, Magma and most back-end dependencies are CMake projects. The new GUI is built beside the
Flutter one in a new `gui/` directory and replaces it once it reaches parity. Two general widgets
the GUI needs, a virtualized grid and a slider, are built in Basalt with full documentation,
examples and tests. A folder picker is added to Magma. The app itself adds image decoding, a
zoom-and-pan view and video playback. Linux uses Magma's Vulkan backend. Screen-reader support is
out of scope. The main risks are the gRPC and TLS parts of the CMake move, video playback, and the
memory target for large folders.

## Scope

The rewrite covers everything under `frontend/` and the build system. The back end's code does
not change.

- **Unchanged:** the back end's sources, `proto/service.proto`, the process model (launch, ready
  handshake, auth token, parent-pid watchdog), the wizard steps S0–S13, the deletion model, and
  session restore.
- **Replaced:** Flutter, Riverpod, the Dart gRPC stubs, `file_picker`, `video_player` and `fvp`,
  and the whole Bazel setup, including `rules_flutter`, the plugin overlays and their patches.
- **Rewritten specs:** `spec/frontend.md` sections 2, 3.3, 4, 5, 9, 12, 13 and 14 describe
  Flutter mechanics and need C++ equivalents. `spec/CONVENTIONS/FLUTTER.md` is retired, and the
  GUI follows `spec/CONVENTIONS/C++.md`. `spec/backend.md` and `AGENTS.md` change where they
  mention Bazel.
- **Out of scope:** a platform accessibility bridge. Basalt widgets still describe themselves
  through `accessibility_info`, so a bridge can be added later without changing the screens.

The Flutter GUI is about 10,000 hand-written lines of Dart, with the largest parts being the
trips review, the wizard state, the detail view and the gRPC client.

## Build System

The repo moves to CMake, following the layout and conventions Basalt and Magma already use. This
happens as the first phase, before any GUI code, so the GUI is built with CMake from its first
line.

### Reasons

- About half of the Bazel complexity exists for Flutter: `rules_flutter`, `rules_dart`, the `fvp`
  plugin overlay, and six of the ten files in `patches/`. All of it goes away with the rewrite.
- Basalt and Magma are CMake projects that fetch pinned dependencies at configure time. Building
  them under Bazel would need `rules_foreign_cc` plus a separate pinned archive for each of their
  dependencies, or new Bazel modules in both libraries.
- Most remaining back-end dependencies are native CMake projects: llama.cpp (built today through
  `rules_foreign_cc`'s `cmake()` rule), OpenCV, curl, spdlog, CLI11, stdexec, gRPC and protobuf.
  SQLite is a single source file. The mdk SDK ships prebuilt libraries.
- The back end's Bazel file is about 220 lines and a dozen targets, so the port is small.
- CMake writes `compile_commands.json` directly, which removes the Hedron extractor step from
  `just lint`.

### Costs

- gRPC and protobuf are slow to build from source, and Bazel's action cache currently hides
  that. A clean CMake build takes noticeably longer. `ccache` and reusing an installed gRPC when
  one is found reduce this.
- Bazel's hermetic toolchain and sandbox go away. Pinned archives with SHA-256 checksums, as in
  Basalt and Magma, keep dependency versions reproducible, but the compiler comes from the
  machine.
- The TLS setup for curl needs a new decision. Today `.bazelrc` points curl at the BoringSSL
  that gRPC brings. Under CMake the likely choice is Schannel on Windows and the BoringSSL or
  OpenSSL that gRPC builds on macOS and Linux.
- The OpenCV patches in `patches/` were written against the Bazel Central Registry port. Some may
  not be needed with OpenCV's own CMake build, and some may need to be redone.
- `tools/package.py` and the Windows installer script locate outputs through `bazel cquery` and
  need rewriting.

### Layout

- A root `CMakeLists.txt` and `CMakePresets.json` with `debug`, `release` and `sanitize`
  presets, building into `.build/<preset>`, as in Basalt and Magma.
- `cmake/dependency_pins.cmake` holds every dependency's URL and SHA-256.
  `cmake/dependencies.cmake` declares them with `FetchContent`, using `FIND_PACKAGE_ARGS` so an
  installed copy is used when present. Basalt and Magma are pinned to exact releases there, and
  Magma uses the Basalt that Kustavi declares.
- `backend/CMakeLists.txt`, `gui/CMakeLists.txt`, and a shared `proto/CMakeLists.txt` that
  generates the C++ protobuf and gRPC sources for both.
- Back-end tests register with CTest. The `just` recipes keep their names and call CMake and
  CTest instead of Bazel.
- The repo's warning flags apply only to Kustavi's own targets, and Basalt and Magma headers are
  included as system headers.

While the Flutter GUI still exists, `just build-gui` builds it with `flutter build` directly, as
before `rules_flutter`.

## Architecture

The GUI is one process with one UI thread and a small number of worker threads. The UI thread
owns all Basalt and Magma objects and all app state. Workers do blocking I/O and decoding and
hand results back through a queue.

### Threads and the event loop

- The main loop follows `magma/examples/event_loop.cpp`: wait for events, feed the frame
  scheduler, dispatch input to the Basalt `event_router`, and render when a frame is due.
- Workers push completed results (stream events, decoded images, process exit) onto a
  mutex-protected queue and call `platform::wake()`. The loop drains the queue each iteration,
  applies the results to app state, and requests a frame.
- Basalt's `text_shaper` is not thread-safe, so only the UI thread builds or measures widgets.
- Workers use `std::jthread` and `std::stop_token`, per the C++ conventions.

### Rendering backends

The window is created with the graphics API that `magma::preferred_graphics_api` gives. That is
Metal on macOS, Vulkan on Linux, and Direct3D 12 on Windows when an adapter is available. Linux
uses Vulkan by default. The raster backend remains the automatic fallback on machines without a
usable GPU, and it renders the golden images in tests.

### Back-end process and gRPC

- Process launch, the `KUSTAVI-READY <port>` handshake, log capture into a 256 KB ring buffer,
  exit watching and clean shutdown move into a small library, `process_supervisor`. The back
  end's `smoke_client` already spawns the server, so the shared code should come from there and
  serve both.
- The GUI uses the same generated C++ gRPC stubs as the back end. A `kustavi_client` class wraps
  them with the same methods as the Dart `KustaviClient` interface. Each streaming call runs on a
  worker that reads the stream and posts events to the UI queue. Cancelling a pass calls
  `ClientContext::TryCancel`.
- gRPC status codes map to a `backend_error` variant (`crashed`, `rpc`, `startup_failed`,
  `not_found`), returned through `std::expected`.
- A `fake_kustavi_client` supports tests, as in the Flutter version.

### App state

Riverpod is replaced by plain value types and a controller on the UI thread.

- The domain types (`image_info`, `quality_flag_info`, `trip_info`, `deletion_plan`, and the
  others in `spec/frontend.md` §5) become C++ structs. `wizard_phase` becomes a `std::variant`.
- The logic that is pure today — deletion resolution order, trip-edit overlays, batch building,
  nearby-shot neighbors, formatting — becomes free functions with unit tests. These are ported
  first because the rest depends on them and because the existing Dart tests describe their
  behavior exactly.
- A `wizard_controller` holds the current state, applies queue results and user actions, and
  tells the screen layer when to rebuild. Screens rebuild their widget subtree from state rather
  than mutating widgets piecemeal, which keeps the model close to the current Flutter one.

### Screens and widgets

Each Flutter screen becomes a function or class that builds a Basalt subtree from state. Most
Flutter widgets have a Basalt counterpart.

| Flutter use | Basalt / app replacement |
| --- | --- |
| Buttons, labels, chips, progress | `button`, `label`, `chip`, `progress_bar` |
| `Switch` | `toggle` |
| `TextField` (destination, folder rename) | `text_field` |
| Collapsible settings panels | `accordion` |
| "Move to trip ▸" menus | `menu` / `popover` |
| Dialogs (crash, log, errors) | `dialog` on `overlay_host` |
| Batch list beside pass cards | `split_view` with a list of selectable rows |
| Step indicator, action bar | App widgets composed from `box` and `stack` |
| `Slider` | New Basalt `slider` |
| `GridView.builder` | New Basalt `grid_view` |
| `InteractiveViewer` | App zoom-and-pan view |
| `SnackBar` | Small toast built on `overlay_host` |
| Material icons | Bundled icon font drawn as text |

Basalt's `font_manager` reads only the fonts the app adds, so the GUI bundles its UI font and the
icon font.

## Library Work

The GUI needs several things the libraries do not provide yet. The grid and the slider are built
in Basalt from the start, and the folder picker in Magma. Each follows its library's rules in
`AGENTS.md` and `spec/CONVENTIONS.md`, and each ships in a tagged release that Kustavi pins.

### Basalt: `slider`

A horizontal slider that follows the Cubit design system, with a range, a step, a value, and an
optional label and value text. It supports drag, click on the track, the arrow keys, Page
Up/Down and Home/End, and the `slider` accessibility role that already exists. It reports changes
while dragging and when the drag ends, so the app can update previews live and re-run work only
on release. The threshold sliders in the quality review need both.

### Basalt: `grid_view`

A virtualized grid for large collections. It knows the item count and the cell size, computes
the column count from its width, and creates widgets only for the visible rows plus a margin. It
recycles cell widgets as they leave the view. The app supplies a factory that builds or updates a
cell for an index, and the grid reports the visible index range so the app can start and cancel
image loads. It supports keyboard navigation, `scroll_into_view` for an index, and an optional
header per section, which the trips review and the final review use for their groups.

Basalt has no image decoder, so the grid works with any cell widget. A plain `image_tile` cell
(image, caption, and a slot for corner badges) can come with it, since most uses of a grid like
this show images. Kustavi's badges and reason chips stay in the app and go in the badge slot.

### Basalt deliverables for each widget

- Public header in `include/basalt/widgets/` and an include in `basalt/basalt.h`.
- Implementation in `src/basalt/widgets/`.
- Catch2 tests in `tests/basalt/`, covering layout, input, focus, accessibility and, for the
  grid, cell creation and recycling at large item counts.
- A documentation page in `docs/widgets/` with the usual sections (create and configure, events,
  notes, example), a link from `docs/widgets/index.md`, and a mention in the README widget list.
- An example program in `examples/widgets/` and a screenshot from `just screenshots`.
- A benchmark for the grid in `benchmarks/widgets_benchmark.cpp`: layout and paint while
  scrolling 50,000 items.

### Magma

- **Folder picker.** `platform` has open-file and save-file dialogs only. A
  `show_open_folder_dialog` method uses SDL 3's `SDL_ShowOpenFolderDialog` and follows the
  existing dialog API. It needs a README entry and a platform test.
- **Quit requests.** Cmd+Q, the macOS app menu and window close must arrive as events the app
  can intercept, so the GUI can shut the back end down cleanly first. This may already work and
  needs checking.
- **Image texture release.** The GPU backends cache a device copy of each image by id. The grid
  creates and drops many images while scrolling, so those copies must be released when the image
  is no longer drawn. If the renderers do not already do this, it is added with a test.

### In the app

- **Image decoding.** A worker pool decodes working images and full-resolution masters into
  `basalt::raster_image`. OpenCV's `imgcodecs` is already built for the back end and is linked
  here. Formats it cannot read fall back to the working image. Decoded images live in a bounded
  LRU cache sized for the memory target.
- **Zoom-and-pan view.** For the detail view: 1×–8× zoom, drag to pan, double-click to reset,
  and a swap from the working image to the master when the master finishes decoding.
- **Video playback.** The detail view plays videos today through `video_player` and `fvp`, which
  use libmdk. The C++ GUI calls libmdk directly, using the same pinned `mdk-sdk` release. Frames
  are copied into images for Basalt to paint, or drawn by mdk into its own surface if Magma can
  expose one. A short spike settles which.

## Phases

Each phase ends with something runnable. The Flutter GUI remains the shipping GUI until phase 8.

1. **CMake migration.** Port the back end, its tests, the `just` recipes, packaging and lint to
   CMake on macOS, Windows and Linux. Exit criteria: `just test-backend`, `just test-quality`,
   `just test-video` and `just package` work, and the Bazel files are removed except what the
   Flutter build still needs, if anything.
2. **Library work and spike.** Build `slider` and `grid_view` in Basalt and the folder picker in
   Magma, and release both libraries. In parallel, a spike app in `gui/` shows the working
   images of `test/photos` in a `grid_view`. Exit criteria: smooth scrolling with several
   thousand images on all three platforms, with Vulkan on Linux.
3. **Foundations.** Process supervisor, `kustavi_client`, the worker-to-UI queue, the domain
   types, and the pure logic with its tests ported from `frontend/test/unit`.
4. **Shell and linear screens.** Wizard shell with step indicator and action bar; S0 start
   (with the model-preparation card), S0-B session restore, S1 scanning, S2 confirm folder, the
   no-images screen, error screens, the crash dialog and the back-end log dialog.
5. **Organize.** S3 trips review: clustering sliders, folders with inline rename, multi-leg
   trips, select mode, move/add menus, the unassigned section, and trip-edit persistence on
   resume. This is the largest screen.
6. **Review.** S4 batch menu, S5 pass running (including junk model download), and the S6
   reviews for quality, junk, video and duplicates, with threshold previews and nearby-shot
   chips.
7. **Detail view and commit.** Detail view with progressive loading, zoom and video. S10 final
   review, S11 commit summary with the debounced estimate, S12 committing and S13 done.
8. **Cut-over.** Packaging and installer changes for the new GUI, spec rewrites, and removal of
   `frontend/`.

The video spike runs alongside phases 3–4 so its result is known before phase 7.

## Testing

Tests run under CTest.

- **Unit tests** cover the pure logic and the controller, driven by `fake_kustavi_client`. They
  port the cases in `frontend/test/unit`, including the deletion fallback order, resume, and
  force-fresh.
- **Widget tests** build screens into a `basalt::widget_tree` without a window, dispatch
  synthetic events, and check the resulting state and tree. This covers the cases in
  `frontend/test/widget`.
- **Image tests** render selected screens with Magma's raster backend into memory and compare
  them with golden PNGs, as Magma's own tests do. These are few and limited to stable layouts.
- **End to end:** the back-end smoke test carries over unchanged apart from its build. A manual
  checklist per phase walks the transitions in `spec/frontend.md` §6.4 against `test/photos` on
  each platform.

## Risks

- **gRPC, protobuf and TLS under CMake.** These are the hardest dependencies to build from
  source with CMake, and curl's TLS backend has to be chosen again. Phase 1 deals with them
  before any GUI work depends on the new build.
- **OpenCV on Windows.** The existing MSVC patches may not carry over directly to OpenCV's own
  CMake build.
- **Video playback.** Driving libmdk directly and getting its frames on screen through Magma is
  unproven. If it does not work well, the detail view can show a still frame and open the video
  in the system player.
- **Memory with large folders.** The < 1 GB RSS target for 50,000 images depends on the grid
  creating only visible cells, the decode cache staying bounded, and the renderers releasing GPU
  copies of dropped images.
- **Vulkan on Linux.** Driver quality varies across distributions. Magma falls back to the
  raster backend when no usable device is found, which limits the impact.
- **Library maturity.** Basalt and Magma are young and change often, and phase 2 puts new
  widgets in front of the GUI work. Pinning exact releases limits the impact, but some GUI phases
  will wait on library releases.
- **Parallel GUIs.** While both GUIs exist, behavior changes need to land in both or be held
  back. Keeping the rewrite short and freezing new GUI features during it reduces this.

## Decisions Recorded

- Screen-reader support is out of scope for this rewrite.
- `grid_view` and `slider` are built in Basalt from the start, with documentation, examples,
  screenshots and tests.
- Linux uses the Vulkan backend by default, with the raster backend as the automatic fallback.
- The repo moves from Bazel to CMake as the first phase.

## Outcome

All eight phases are done on macOS, and `frontend/` is gone. The GUI is `kustavi_gui`, built by
the same CMake project as the back end, and `just package` produces `Kustavi.app` and a zip from
it. Windows and Linux are not tested, and Basalt and Magma are not released or pinned.

What was built:

- The back end builds with CMake, and the Bazel files are gone.
- Basalt got `slider`, `grid_view` and `image_tile`, and Magma got a folder chooser and a bounded
  image cache. Both changes are committed locally in the two repositories and not tagged.
- `gui/core` holds the logic without any window: domain types, the deletion model, the trips
  plan, the gRPC client, the back-end host, `wizard_controller`, and their Catch2 tests, which
  port the Dart unit tests.
- `gui/app` holds the screens for every step S0 to S13, the detail view with zoom and video, the
  crash and log dialogs, and `kustavi_gui_preview`, which draws a screen to a PNG.
- Video uses libmdk through frame copies. The spike showed that this is fast enough: the player
  asks mdk for frames scaled to at most 1600 px, so a 4K clip costs no more than a window-sized
  one.
- Packaging assembles the macOS bundle with an icon, `Info.plist`, libmdk in `Frameworks` and an
  ad hoc signature, or the Windows folder for the WiX installer. OpenCV and curl are configured
  so that the bundle needs only system libraries.

Differences from the Flutter app and gaps:

- The UI uses system fonts, and the bundled UI and icon fonts that this plan describes are not
  done. Buttons carry words, not icons.
- Screen readers are out of scope. Widgets describe themselves, so a bridge can be added later.
- The app is light-themed only.
- A video has Play and Pause, with no seek bar and no sound controls.
- The duplicate review makes a clicked photo the keeper, and it has no per-photo toggles beyond
  that.
- The GUI keeps the bundle identifier of the Flutter app, `com.example.frontend`, so that the
  folder access that macOS granted to it carries over. The identifier should change before a
  public release.
- Basalt and Magma must be pinned to tagged releases before a build can be reproduced without
  local checkouts. The Windows and Linux packaging, the Vulkan and Direct3D 12 changes in Magma
  and the Windows child-process code have not been compiled.
