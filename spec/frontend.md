# Front-End Specification (Kustavi GUI)

## 1. Overview

The Kustavi GUI is a Flutter desktop application that drives a linear
wizard over a folder of images:

1. Select folder (with visual confirmation grid)
2. Quality pass — blurry / poorly exposed images
3. Junk pass — screenshots and non-photographs (local vision LLM)
4. Duplicates pass — similar-image groups, keep-one selection
5. Trips pass — spatiotemporal grouping with user thresholds
6. Commit — copy the kept images to a user-chosen destination

All user input is handled here. All image processing lives in the C++
back end, which the GUI launches, supervises, and talks to over gRPC on a
loopback TCP port. The normative API is defined in `spec/proto.md`.
Back-end behavior is specified in `spec/backend.md`.

State is session-only: nothing the user decides is persisted. Closing the
app mid-session discards all decisions.

## 2. Platforms & Build

- Targets: macOS, Windows, Linux desktops (see `os_name`/`flutter_target`
  in the root `.justfile`).
- Built with `flutter build <target>`; the `just run` recipe builds and
  launches it.
- `pubspec.yaml` dependencies (in addition to the existing riverpod/grpc
  stack): `file_picker` (directory pickers), `path` (path display/manip).
- Generated gRPC stubs live in `lib/src/generated/` (checked in, see §13).
- Riverpod providers use code generation (`@riverpod`); generated `.g.dart`
  files are checked in.
- `analysis_options.yaml` must adopt the strict analyzer + lint block from
  `spec/CONVENTIONS/FLUTTER.md` §7. `flutter analyze` must be clean.
- Window: default 1280×800, minimum 960×640. English-only UI.

## 3. Process Model

### 3.1 Launching the back end

The GUI is the supervising process. On app start it launches exactly one
back-end process.

Binary discovery, in order:

1. `KUSTAVI_BACKEND_PATH` environment variable (absolute path) — used in
   development to point at the Bazel output (`bazel-bin/backend/server`).
2. `kustavi-backend` (or `kustavi-backend.exe`) in the same directory as
   the running GUI executable. On macOS this is
   `<App>.app/Contents/MacOS/kustavi-backend`.

If the binary is not found, the GUI shows a start-up error screen
("Back end not found. Set KUSTAVI_BACKEND_PATH or install the back end
next to the app.") with a single [Exit] button. The wizard cannot start
without the back end.

Launch arguments: `--listen 127.0.0.1:0` (port 0 = OS-assigned).

Ready handshake:

1. After binding its socket, the back end prints exactly one line to
   stdout, flushed immediately: `KUSTAVI-READY <port>\n`.
2. The GUI waits up to 30 s for that line, opens a gRPC channel to
   `127.0.0.1:<port>`, and calls `GetInfo` (5 s deadline) as a readiness
   probe.
3. If the line does not arrive, is malformed, or `GetInfo` fails: start-up
   error screen with [Retry] (relaunch) and [Exit].

All other back-end stdout/stderr lines are captured into an in-memory
ring buffer (256 KB) and shown in a "Back end log" dialog reachable from
the app menu.

### 3.2 Supervision

- The GUI watches the back-end process for exit.
- Unexpected exit while the app is alive triggers the crash dialog (§10.1).
- Clean shutdown (app quit or after the Done screen): GUI calls
  `Shutdown`, waits up to 3 s for process exit, then kills the process if
  it is still alive.
- The GUI never runs more than one back-end at a time. "Retry" after a
  crash starts a fresh process and repeats the ready handshake.

### 3.3 Transport

- gRPC over HTTP/2, insecure credentials, loopback TCP only. (Unix domain
  sockets are out of scope: the Dart `grpc` package does not support
  them.)
- The `ClientChannel` is owned by a Riverpod provider and shut down in
  `ref.onDispose`, per `spec/CONVENTIONS/FLUTTER.md` §6.

## 4. API Integration

The GUI consumes `spec/proto.md` through a single wrapper interface so
screens and the wizard controller never touch generated stubs directly:

```dart
abstract interface class KustaviClient {
  Future<GetInfoResponse> getInfo();
  Future<void> shutdown();
  Stream<ScanEvent> scanFolder(ScanFolderRequest request);
  Stream<QualityEvent> runQualityPass();
  Stream<ModelEvent> ensureModel();
  Stream<JunkEvent> runJunkPass();
  Stream<SimilarEvent> runSimilarPass();
  Stream<TripsEvent> runTripsPass(TripsPassRequest request);
  Stream<CommitEvent> commit(CommitRequest request);
}
```

- The production implementation wraps the generated gRPC client and maps
  stream errors to `BackendError` (§10.5). Returned streams wrap
  cancellable gRPC calls; cancelling a stream in the GUI cancels the
  back-end pass (proto spec §2).
- A `FakeKustaviClient` (in-memory) exists for tests.
- At most one pass RPC is in flight at a time. The GUI must not start a
  new pass stream until the previous one has completed.

## 5. Domain Model (Dart)

Plain immutable Dart types, built from proto events:

- `ImageInfo`: id, path, name, size, width, height, `DateTime? taken`,
  `(double, double)? gps`, `String thumbnailPath`.
- `QualityFlagInfo`: image id, reasons (`Blurry`, `UnderExposed`,
  `OverExposed`), sharpness, exposure score.
- `JunkFlagInfo`: image id, reason text, confidence.
- `SimilarGroupInfo`: id, member ids, recommended keep id, per-member
  scores.
- `TripInfo`: id, start/end `DateTime`, member ids, centroid `(double,
  double)?`.
- `DeletionPlan`: the `Set<String> marked` image ids, plus derivation
  helpers (§8).
- `WizardPhase`: sealed class hierarchy (§6).

Errors are values: `sealed class BackendError` with subtypes
`Crashed`, `Rpc(GrpcError)`, `StartupFailed(String)`, `NotFound(String)`.

The image index (`Map<String, ImageInfo>`) is built incrementally from
`ScanFolder` events and kept in the wizard controller. It is the
single source of truth for image metadata in the GUI.

## 6. The Wizard

### 6.1 Step indicator

A top bar shows the six steps — Select, Quality, Junk, Duplicates, Trips,
Copy — with the current step highlighted and completed steps checked.
The indicator is display-only; navigation is via the bottom action bar
([Back] / [Cancel] / [Continue]-style buttons per step).

### 6.2 Steps

**S0 — Start.**
UI: app title, [Select folder…] button. If the vision model is still
preparing in the background (§6.3), a small card shows
"Preparing vision model — 42% (1.2 / 2.9 GB)".
Action: directory picker (`file_picker`); on selection, call
`ScanFolder` and enter S1.

**S1 — Scanning.**
UI: indeterminate-ish progress: images found so far, current path being
scanned, [Cancel].
Exit: `ScanComplete` → S2. Zero images found → "No images found in
<folder>" screen with [Choose another folder] (→ S0) and [Exit].
Cancel → S0.

**S2 — Confirm folder.**
UI: header "<N> images in <folder>"; grid of every image (thumbnails +
file names); [Back] (→ S0, discards scan results) and [Continue] (→ S3).
Clicking an image opens the detail view (§7.2) in read-only mode
(metadata only, no deletion toggle).

**S3 — Quality running.**
Entry: `RunQualityPass`.
UI: progress "checking 1,204 / 5,000", current file name, [Cancel].
Exit: complete → S4. Cancel → S2. RPC error → §10.2.

**S4 — Quality review.**
Shows only flagged candidates.
UI: header "<X> of <N> images flagged". Grid of flagged images only; each
cell shows thumbnail, reason chips ("Blurry", "Overexposed"), and a
deletion toggle (default ON = marked for deletion). Cell click → detail
view with the deletion toggle enabled.
Buttons: [Keep all] (clears marks on all flagged), [Mark all] (marks all
flagged), [Back] (→ S2), [Continue] (→ S6, via S5 if the model is not
ready).
When X = 0 the screen shows "No blurry or poorly exposed images found"
and only [Continue].

**S5 — Junk preparation (transient).**
If the vision model (§6.3) is not ready when the user reaches the junk
pass, show a "Downloading vision model" screen with byte progress, speed,
and [Cancel].
- Cancel → back to S4. The download is cancelled; `EnsureModel` runs
  again the next time the user continues from S4.
- Download fails → error screen with [Retry download] and [Exit app].
  There is no skip option: the junk pass always uses the LLM.
- Model becomes ready → S6 automatically.

**S6 — Junk running.**
Entry: `RunJunkPass` (only once the model is ready).
UI: progress "1,204 / 5,000", elapsed time and ETA (the LLM pass is slow;
make that expectation explicit in the copy), [Cancel].
Exit: complete → S7. Cancel → S4. Zero flagged → S7 shows its
"nothing flagged" variant.

**S7 — Junk review.**
Same pattern as S4; reason chips show the VLM classification (e.g.
"screenshot", "scan"). [Continue] → S8.

**S8 — Similar running.**
Entry: `RunSimilarPass`.
UI: progress, [Cancel] (→ S7).
Exit: complete → S9.

**S9 — Similar review.**
Dedicated group view.
UI: header "<G> groups · <K> photos marked for deletion".
Vertical list of group cards:
- Card header: "Group <i> — <M> similar photos".
- A row of member cells (thumbnail, file name, score); the recommended
  keeper is pre-selected with a "Suggested" badge. Selecting a different
  member reassigns the keeper.
- Card actions: [Keep all] (clears marks on all members) and [Delete all]
  (marks all members).
- Cell click → detail view with the deletion toggle enabled.
Marks update live in `DeletionPlan` (§8) as the user interacts.
[Back] (→ S7), [Continue] (→ S10).
When G = 0: "No similar photos found" + [Continue].

**S10 — Trips.**
Entry: `RunTripsPass` with the current slider values (defaults: 48 h,
300 km). Re-clustering runs on slider release; a brief "Clustering…"
indicator is shown and previous results are replaced.
UI, two panes:
- Left: sliders — "Max time gap: <n> h" (1–168 h) and "Max distance:
  <n> km" (10–1000 km) — and the trip list, one row per trip:
  "<start date> – <end date> · <n> photos" (plus a location marker if GPS
  is available). If `unassigned > 0`, a row "No timestamp · <n> photos".
- Right: grid of the selected trip's members in chronological order, with
  deletion badges; cell click → detail view with the deletion toggle
  enabled. The trips step is organizational: per-image decisions are
  made through the detail view, never in bulk.
[Back] (→ S9), [Continue] (→ S11).

**S11 — Commit summary.**
UI: "Keep <N> photos (<total size>) — <M> will be left behind".
Destination: text field + [Choose folder…] (directory picker; the
suggested default, pre-filled, is a sibling of the source named
`<source-name>-kept`). [Back] (→ S10). [Copy] (disabled until a
destination is set) → S12.

**S12 — Committing.**
Entry: `Commit` with `destination` and `keep_ids = all − marked`.
UI: progress "<done> / <total>", current file name, [Cancel] (→ S11;
partially copied files remain — re-committing is safe, see proto spec).
Exit: `CommitComplete` → S13.

**S13 — Done.**
UI: "Copied <n> photos to <destination>". If there were skips or errors,
they are listed. Note: "Your original files were not modified. You can
safely delete the source folder."
Buttons: [Start over] (resets the wizard to S0 in the same app session,
starting a new back-end session) and [Done] (shuts down the back end and
quits the app).

### 6.3 Vision model preparation

- On app start (S0), the GUI begins `EnsureModel` as a background
  provider. Model states: `unknown → downloading(progress) → ready` or
  `failed(message)`.
- `ready` is cached for the session. `EnsureModel` is called again only
  after a user-requested retry or after the user cancelled the S5
  download screen (which cancels the stream and resets the state to
  `unknown`).
- S5 awaits the same provider and renders its state.
- The model name/size come from the `GetInfo`/`ModelReady` events; the
  GUI displays them but makes no decisions about the model itself.

### 6.4 Transition table

| From | Trigger | To |
|---|---|---|
| S0 | folder selected | S1 |
| S1 | scan complete, images > 0 | S2 |
| S1 | scan complete, 0 images | no-images screen → S0 |
| S1 | cancel | S0 |
| S2 | Back | S0 |
| S2 | Continue | S3 |
| S3 | complete | S4 |
| S3 | cancel | S2 |
| S3 | RPC error | error screen (§10.2) → S2 or exit |
| S4 | Continue | S5 (if model not ready) else S6 |
| S4 | Back | S2 |
| S5 | model ready | S6 |
| S5 | cancel | S4 |
| S5 | download failed | error screen → retry (S5) or exit |
| S6 | complete | S7 |
| S6 | cancel | S4 |
| S7 | Continue | S8 |
| S7 | Back | S4 |
| S8 | complete | S9 |
| S8 | cancel | S7 |
| S9 | Continue | S10 |
| S9 | Back | S7 |
| S10 | Continue | S11 |
| S10 | Back | S9 |
| S11 | Copy | S12 |
| S11 | Back | S10 |
| S12 | complete | S13 |
| S12 | cancel | S11 |
| S13 | Start over | S0 (fresh session) |
| S13 | Done | app quit |

Back navigation never re-runs a completed pass; it revisits the previous
step's screen with its stored results. Cancelled passes are re-run from
scratch when the user continues forward again.

## 7. Shared UI

### 7.1 Image grid

- `GridView.builder` (lazy), adaptive column count targeting ~176 px
  cells.
- Cell: thumbnail (`Image.file` on `thumbnailPath`), one-line ellipsized
  file name, badges: red "delete" tag when the id is in `marked`,
  reason chips in review steps, "keeper" badge in S9.
- Badges reflect `DeletionPlan` live.
- Cell click → detail view. No multi-select anywhere: the tool's purpose
  is to keep the user from manually triaging images, so every bulk
  decision is made by the back end and refined per image.

### 7.2 Detail view

Modal over the current grid:

- Zoomable original image (`InteractiveViewer`, 1×–8×, double-click
  resets). Loads the original file from `ImageInfo.path`; evicted from
  the image cache when closed.
- Metadata panel: name, path, dimensions, file size, date taken (if
  known), GPS coordinates (if known), sharpness/exposure (if computed),
  junk reason (if flagged), similar-group / trip membership.
- "Marked for deletion" switch: enabled in S4, S7, S9, S10; disabled
  (read-only) in S2.
- Close: [Close] button, Esc key, or click outside.

## 8. Deletion Model

`DeletionPlan` holds the single source of truth, `Set<String> marked`.

Defaults are applied when a review step is entered:

- S4: every quality-flagged image is pre-marked.
- S7: every junk-flagged image is pre-marked.
- S9: for each similar group, every member except the selected keeper is
  marked — unless the user explicitly un-marked that member in S4 or S7
  (explicit user choices are recorded in `explicitKept: Set<String>` and
  are respected by the S9 defaults).

User actions mutate `marked` directly: review toggles, detail-view
switch, per-group [Keep all]/[Delete all], keeper reassignment (old
keeper becomes marked, new keeper becomes unmarked).

At S11/S12: `keep_ids = allImages − marked`. The GUI never tells the
back end to delete anything.

## 9. State Management (Riverpod)

Per `spec/CONVENTIONS/FLUTTER.md`: `@riverpod` code generation, sealed
state classes, functional providers, `AsyncNotifier` for async state, no
`setState` for app state.

Providers:

- `backendProcessProvider` — launches the process, performs the ready
  handshake, exposes the port; starts on app start.
- `grpcChannelProvider` — `ClientChannel` per conventions §6.
- `kustaviClientProvider` — `KustaviClient` (gRPC implementation).
- `modelStatusProvider` (`AsyncNotifier`) — owns the `EnsureModel`
  stream, exposes `ModelState`.
- `wizardProvider` (`AsyncNotifier`, the `WizardController`) — owns:
  current `WizardPhase`, the image index, per-pass results
  (`qualityFlags`, `junkFlags`, `similarGroups`, `trips`), `DeletionPlan`,
  group keeper/mode selections, trip thresholds, commit state, and
  per-step progress. Methods: `selectFolder`, `continueStep`, `goBack`,
  `cancelStep`, `toggleMark`, `setGroupKeeper`, `setGroupMode`,
  `setTripThresholds`, `chooseDestination`, `startCommit`, `retry`,
  `restartBackend`, `startOver`.
- `backendLogProvider` — the stdout/stderr ring buffer.

Screens are `StatelessWidget`s that read derived views from
`wizardProvider` (e.g. `markedImages`, `flaggedQualityImages`,
`groupsWithMarks`, `tripMembers(tripId)`). Widgets that cannot be `const`
are split into isolated `StatelessWidget` classes, not `_buildX` helpers
(conventions §5).

## 10. Error Handling

### 10.1 Back-end crash

A modal dialog over whatever step is active: "Processing error: the
back end stopped unexpectedly."
- [Retry]: relaunch the back end (fresh process, ready handshake,
  `GetInfo`), then resume: if the app was in a running step (S1, S3, S5,
  S6, S8, S12), that pass is re-run from scratch; if the app was in a
  completed step, the step simply continues (all results live in GUI
  memory).
- [Exit app]: quit.

### 10.2 Step failure

A pass RPC that terminates in a non-cancel gRPC error (e.g. the source
folder was deleted): an error screen with the back-end message and
[Back] (to the step preceding the failed one) and [Exit app].

### 10.3 Model failure

Covered by S5 (§6.2): [Retry download] or [Exit app]. No skip.

### 10.4 App quit mid-session

Closing the window while the wizard is in any step other than S0 or S13
shows a confirmation: "Your progress will be lost." [Quit] / [Cancel].
On quit: `Shutdown` + process cleanup (§3.2).

### 10.5 Error mapping

gRPC status → `BackendError`: `NOT_FOUND` → `NotFound`;
`UNAVAILABLE` during a call → treated as crash (10.1); anything else →
`Rpc` with the status message surfaced verbatim.

## 11. Performance & Memory (16 GB machine)

- The GUI never decodes full-size images except one at a time in the
  detail view. Grid cells render back-end-generated thumbnails (JPEG,
  ≤ 256 px long edge, ~10–30 KB each).
- All grids are lazy (`GridView.builder`); the detail image is evicted
  from the cache on close.
- Target: < 1 GB GUI RSS with a 50,000-image folder (the image index of
  50 k `ImageInfo` records is a few tens of MB).
- The junk pass is the long pole: the GUI shows current/total, elapsed
  time, and ETA, and always offers [Cancel].
- The GUI must remain responsive during all back-end passes (all work is
  in the back-end process; streams are consumed asynchronously).

## 12. Testing

Required (run with `just test-gui`, a recipe that runs
`cd frontend && flutter analyze && flutter test`):

- Unit tests (pure Dart, using `FakeKustaviClient`):
  - wizard controller transitions for every row of §6.4, including
    cancel, back, crash-retry, and start-over;
  - `DeletionPlan` derivation: review defaults, `explicitKept`
    interaction, group keeper reassignment, [Keep all]/[Delete all],
    final `keep_ids` computation;
  - trip label formatting (date ranges, counts);
  - ETA computation.
- Widget tests:
  - S0 → S1 → S2 flow with a fake client (grid renders, count header);
  - S4: toggling a flag, [Keep all], [Continue] advances;
  - S9: selecting a different keeper updates marks; [Keep all] clears
    them;
  - S10: moving a slider triggers `RunTripsPass` with the new values;
  - S11 → S12 → S13: progress renders, [Done] path;
  - crash dialog: [Retry] restarts the back end, [Exit] quits;
  - S5: download progress and failure screens.

## 13. Dependencies & Code Generation

- `pubspec.yaml`: add `file_picker` and `path`.
- Dart gRPC stubs: generated with `protoc` + `protoc_plugin` into
  `frontend/lib/src/generated/kustavi/` and checked in. Regenerate with
  the `just proto` recipe:
  `protoc -I proto --dart_out=frontend/lib/src/generated --grpc_out=frontend/lib/src/generated proto/service.proto`
- Riverpod code generation: `flutter pub run build_runner build` in
  `frontend/`; output checked in.
- `proto/service.proto` is the single source of truth for the API; see
  `spec/proto.md` §6 for the C++ side.

## 14. Directory Layout

```
frontend/lib/
  main.dart                 # entrypoint, ProviderScope, window setup
  app.dart                  # MaterialApp, theming
  src/
    backend/
      process.dart          # launch, ready handshake, supervision, log ring buffer
      channel.dart          # grpcChannelProvider
      client.dart           # KustaviClient interface + gRPC implementation
      fake_client.dart      # in-memory client for tests (dev dependency path)
    generated/              # protoc output (checked in)
    state/
      phases.dart           # sealed WizardPhase
      domain.dart           # ImageInfo, flags, groups, trips, BackendError
      decisions.dart        # DeletionPlan
      model_status.dart     # modelStatusProvider
      wizard.dart           # WizardController (AsyncNotifier)
    ui/
      wizard_shell.dart     # step indicator, action bar, screen routing
      start.dart
      scanning.dart
      confirm_folder.dart
      quality_review.dart
      junk_prep.dart
      junk_review.dart
      similar_review.dart
      trips.dart
      commit_summary.dart
      committing.dart
      done.dart
      errors.dart           # crash dialog, step error screen, no-images screen
      widgets/
        image_grid.dart
        image_cell.dart
        detail_view.dart
        badges.dart
        progress.dart
frontend/test/
  unit/
  widget/
```

## 15. Decisions Recorded

- Scanning is recursive (subfolders included) by default.
- The destination folder is always user-chosen; the pre-filled suggestion
  is a sibling directory named `<source-name>-kept`.
- Cancelling a running pass returns to the preceding step; the pass is
  re-run from scratch if the user continues forward again.
- The junk pass always uses the LLM; there is no OpenCV-only mode and no
  user-facing model configuration.
- The vision model download starts in the background on app start so it
  is usually finished by the time the junk pass is reached.
- Bulk decisions exist only as per-step actions ([Keep all]/[Mark
  all]/[Delete all]); there is no free-form multi-select.
- The trips step is organizational: it shows spatiotemporal groups and
  allows per-image decisions via the detail view, but no bulk actions.
