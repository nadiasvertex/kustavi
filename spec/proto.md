# Back-End RPC Specification (gRPC)

## 1. Overview

Kustavi's front end and back end communicate over gRPC on a loopback TCP
port. This document is the normative API the front end expects (see
`spec/frontend.md`); the C++ back end is specified in `spec/backend.md`
and must implement exactly this contract.

- Single source of truth: `proto/service.proto`, package `kustavi`.
- One service: `Kustavi`. All long-running operations are
  server-streaming methods that emit `oneof`-based event messages.
- The back end serves exactly one folder at a time ("the session").
  `ScanFolder` starts the session; every other method requires an active
  session and operates on it. Calling a non-`ScanFolder` method before a
  `ScanFolder` (or calling `ScanFolder` while a pass is running) is
  rejected with `FAILED_PRECONDITION`.

## 2. Transport & Lifecycle

- The GUI launches the back end with `--listen 127.0.0.1:0`.
- After binding, the back end prints one flushed line to stdout:
  `KUSTAVI-READY <port>\n`, where `<port>` is the port it actually bound.
  The GUI waits for this line (30 s timeout) before opening a channel to
  `127.0.0.1:<port>`.
- HTTP/2, insecure credentials, loopback only. No TLS, no auth: the
  listener is bound to 127.0.0.1 and exists only while the GUI runs.
- At most one pass (quality/junk/similar/trips/commit) runs at a time.
  The GUI enforces this with its linear wizard; the back end rejects a
  second concurrent pass with `FAILED_PRECONDITION`.
- Cancellation: the GUI cancels the gRPC call to stop a pass. The back
  end must stop producing new work within 2 s of observing the cancel —
  including aborting in-flight LLM inference — and release its resources.
  A client-cancelled stream is not an error condition for the GUI.
- `Shutdown` must be idempotent and must cause the process to exit
  shortly after the response is sent.

## 3. Conventions

- **Image id** — the path of the image relative to the session folder,
  using `/` as separator (e.g. `2024/paris/IMG_0001.jpg`). Stable for the
  lifetime of the session; all result messages reference images by id.
- **Timestamps** — milliseconds since the Unix epoch, `int64`.
- **GPS** — decimal degrees, WGS84, `(latitude, longitude)`.
- **Optional metadata** — fields that may be missing (taken, gps) are
  proto3 `optional` so presence is meaningful; unset when the data is
  unavailable.
- **Event streams** — every pass stream ends with a `*Complete` event on
  success. Failure is signaled by a non-OK gRPC status (with a
  human-readable message); the GUI does not parse error events.
  Cancellation ends the stream without a complete event.
- **Thumbnails** — JPEG, ≤ 256 px on the long edge. The file at
  `ImageMeta.thumbnail_path` must be fully written before the
  `ImageMeta` event is emitted. Thumbnail paths are valid for the
  lifetime of the session.
- **Status codes** — `INVALID_ARGUMENT` (bad paths/values),
  `NOT_FOUND` (folder missing), `FAILED_PRECONDITION` (no session, pass
  already running, model missing), `INTERNAL` (processing failure),
  `CANCELLED` (client cancel).
- The back end never modifies, moves, or deletes source files. `Commit`
  only copies.

## 4. The Service

```proto
syntax = "proto3";

package kustavi;

service Kustavi {
  // --- lifecycle -------------------------------------------------------
  rpc GetInfo(GetInfoRequest) returns (GetInfoResponse);
  rpc Shutdown(ShutdownRequest) returns (ShutdownResponse);

  // --- pass 1: scan & thumbnails ---------------------------------------
  rpc ScanFolder(ScanFolderRequest) returns (stream ScanEvent);

  // --- pass 2: blur / exposure -----------------------------------------
  rpc RunQualityPass(RunQualityPassRequest) returns (stream QualityEvent);

  // --- pass 3: junk (vision LLM) ----------------------------------------
  rpc EnsureModel(EnsureModelRequest) returns (stream ModelEvent);
  rpc RunJunkPass(RunJunkPassRequest) returns (stream JunkEvent);

  // --- pass 4: similar images -------------------------------------------
  rpc RunSimilarPass(RunSimilarPassRequest) returns (stream SimilarEvent);

  // --- pass 5: trips -----------------------------------------------------
  rpc RunTripsPass(RunTripsPassRequest) returns (stream TripsEvent);

  // --- pass 6: commit ----------------------------------------------------
  rpc Commit(CommitRequest) returns (stream CommitEvent);
}

// --- lifecycle ---------------------------------------------------------

message GetInfoRequest {}

message GetInfoResponse {
  string version = 1;              // back end version
  repeated string supported_formats = 2;  // lowercase extensions, e.g. "jpg"
  string model_name = 3;           // vision model used by the junk pass
}

message ShutdownRequest {}
message ShutdownResponse {}

// --- shared -------------------------------------------------------------

message GpsPoint {
  double latitude = 1;
  double longitude = 2;
}

message ImageMeta {
  string id = 1;               // relative path from the session folder
  string path = 2;             // absolute path to the original file
  string name = 3;             // file name
  uint32 width = 4;
  uint32 height = 5;
  uint64 size_bytes = 6;
  optional int64 taken_unix_ms = 7;
  optional GpsPoint gps = 8;
  string thumbnail_path = 9;   // absolute path, written before emit
}

// --- pass 1: scan --------------------------------------------------------

message ScanFolderRequest {
  string folder = 1;           // absolute path to the source folder
  bool recursive = 2;          // include subfolders
}

message ScanEvent {
  oneof event {
    ScanProgress progress = 1;
    ImageMeta image = 2;
    ScanComplete complete = 3;
  }
}

message ScanProgress {
  uint32 files_seen = 1;
  uint32 images_found = 2;
  string current_path = 3;
}

message ScanComplete {
  uint32 images = 1;
  uint32 skipped_files = 2;    // non-image files
  repeated string errors = 3;  // unreadable image files: "<id>: <reason>"
}

// --- pass 2: quality -------------------------------------------------------

message RunQualityPassRequest {}

message QualityEvent {
  oneof event {
    QualityProgress progress = 1;
    QualityFlag flag = 2;      // emitted for flagged images only
    QualityComplete complete = 3;
  }
}

message QualityProgress {
  uint32 done = 1;
  uint32 total = 2;
}

enum QualityReason {
  QUALITY_REASON_UNSPECIFIED = 0;
  BLURRY = 1;
  UNDER_EXPOSED = 2;
  OVER_EXPOSED = 3;
}

message QualityFlag {
  string image_id = 1;
  repeated QualityReason reasons = 2;  // one or more
  double sharpness = 3;      // Laplacian variance; higher = sharper
  double exposure_score = 4; // 0..1; 0.5 = ideal, lower = worse balance
}

message QualityComplete {
  uint32 flagged = 1;
  uint32 total = 2;
}

// --- pass 3: model + junk ---------------------------------------------------

message EnsureModelRequest {}

message ModelEvent {
  oneof event {
    ModelReady ready = 1;
    ModelDownloadProgress progress = 2;
  }
}

message ModelReady {
  string model_name = 1;
  uint64 size_bytes = 2;
}

message ModelDownloadProgress {
  uint64 done_bytes = 1;
  uint64 total_bytes = 2;
  double speed_bps = 3;
}

message RunJunkPassRequest {}

message JunkEvent {
  oneof event {
    JunkProgress progress = 1;
    JunkFlag flag = 2;         // emitted for junk images only
    JunkComplete complete = 3;
  }
}

message JunkProgress {
  uint32 done = 1;
  uint32 total = 2;
}

message JunkFlag {
  string image_id = 1;
  string reason = 2;    // short classification, e.g. "screenshot", "scan"
  double confidence = 3;  // 0..1
}

message JunkComplete {
  uint32 flagged = 1;
  uint32 total = 2;
}

// --- pass 4: similar -----------------------------------------------------------

message RunSimilarPassRequest {}

message SimilarEvent {
  oneof event {
    SimilarProgress progress = 1;
    SimilarGroup group = 2;
    SimilarComplete complete = 3;
  }
}

message SimilarProgress {
  uint32 done = 1;
  uint32 total = 2;
}

message SimilarGroup {
  uint32 id = 1;                       // stable index within the run
  repeated string image_ids = 2;       // members, best-first
  string recommended_keep_id = 3;
  repeated double member_scores = 4;   // "bestness", parallel to image_ids
}

message SimilarComplete {
  uint32 groups = 1;
  uint32 total_images = 2;
}

// --- pass 5: trips ----------------------------------------------------------------

message RunTripsPassRequest {
  int32 max_gap_hours = 1;    // time gap that ends an away trip (default 48)
  int32 max_distance_km = 2;  // centroid drift that ends an away trip (default 300)
  int32 home_radius_km = 3;   // distance from a home cluster that counts as away (default 15)
  int32 leg_radius_km = 4;    // GPS jump that starts a new leg within a trip (default 25)
}

message TripsEvent {
  oneof event {
    TripsProgress progress = 1;
    Trip trip = 2;
    TripsComplete complete = 3;
  }
}

message TripsProgress {
  uint32 done = 1;
  uint32 total = 2;
}

message Trip {
  uint32 id = 1;
  int64 start_unix_ms = 2;
  int64 end_unix_ms = 3;
  repeated string image_ids = 4;  // chronological order
  optional GpsPoint centroid = 5; // mean of members with GPS; unset if none
  optional string folder = 6;     // display folder name for grouping trips
  string folder_slug = 7;         // filesystem-safe form of folder (commit layout)
  string place_name = 8;          // dominant place ("Italy"); empty without geo
  repeated TripLeg legs = 9;      // contiguous stays; one entry unless the trip has legs
  bool is_home = 10;              // photos taken near a detected home cluster
}

message TripLeg {
  string place_name = 1;          // "Rome, Italy"; empty without geo
  string slug = 2;                // filesystem-safe; unique within the trip
  repeated string image_ids = 3;  // chronological order
  optional GpsPoint centroid = 4; // mean of members with GPS; unset if none
}

message TripsComplete {
  uint32 trips = 1;
  uint32 unassigned = 2;  // images without a usable timestamp
}

// --- pass 6: commit ------------------------------------------------------------------

message CommitRequest {
  string destination = 1;        // absolute path; created if missing
  repeated string keep_ids = 2;  // image ids to copy
  map<string, string> folder_for_id = 3;  // optional per-image destination sub-path (trip/leg slug)
}

message CommitEvent {
  oneof event {
    CommitProgress progress = 1;
    CommitComplete complete = 2;
  }
}

message CommitProgress {
  uint32 done = 1;
  uint32 total = 2;
  string current_name = 3;
}

message CommitComplete {
  uint32 copied = 1;
  uint32 skipped = 2;        // name collisions (different content)
  repeated string errors = 3;  // "<id>: <reason>"
}
```

## 5. Method Semantics

### GetInfo

No preconditions. Returns the back-end version, the list of supported
image formats, and the configured vision model name. The GUI calls this
as the readiness probe after the ready handshake.

### Shutdown

Idempotent. Flushes and deletes the session's thumbnail cache, then the
process exits shortly after the response.

### ScanFolder

Starts (or replaces) the session. Preconditions: no pass currently
running.

- Walks `folder` (recursively if `recursive`), reading EXIF
  (timestamp, GPS) and generating a thumbnail for every image of a
  supported format.
- Supported formats (minimum): `jpg`, `jpeg`, `png`, `heic`, `heif`,
  `webp`, `tiff`, `tif`, `bmp`, `gif` (first frame). RAW formats are out
  of scope.
- Emits `ScanProgress` periodically (at least once per 250 files), one
  `ImageMeta` per image (in walk order, thumbnail written to disk first),
  then `ScanComplete`.
- Unreadable image files are not emitted as `ImageMeta`; they are counted
  in `ScanComplete.errors` as `"<id>: <reason>"`.
- A new `ScanFolder` discards the previous session's index and cache.

### RunQualityPass

Preconditions: active session; no pass running.

- Computes, for every image: sharpness (Laplacian variance) and an
  exposure score from the luminance histogram.
- Thresholds for flagging are the back end's (documented in
  `spec/backend.md`); the GUI is unaware of them.
- Emits `QualityFlag` for flagged images only (with the metric values so
  the GUI can display them), `QualityProgress` along the way, and
  `QualityComplete` (flagged/total counts).
- Deterministic: re-running after a cancel produces the same results.

### EnsureModel

Idempotent; the only method that may download from the network.

- If a valid model file already exists (size + checksum verified),
  emits `ModelReady` immediately and closes the stream.
- Otherwise downloads the model's weights from the configured
  Hugging Face URL (back end's responsibility) and emits
  `ModelDownloadProgress` at least every 0.5 s (with `total_bytes` known
  once the Content-Length is available; 0 if unknown), then
  `ModelReady`.
- Failure (network error, checksum mismatch, disk full): the stream ends
  with status `INTERNAL` and a human-readable message; no `ModelReady`
  is emitted. Partial downloads are resumed or restarted at the back
  end's discretion.
- If the client cancels the stream, the back end may stop or continue
  the download; the next `EnsureModel` call resumes or restarts it.
- The model is stored in the platform application-data directory under a
  `kustavi` subdirectory (e.g. `~/Library/Application Support/kustavi`
  on macOS, `%APPDATA%\kustavi` on Windows,
  `$XDG_DATA_HOME/kustavi` on Linux). The GUI never touches these files.

### RunJunkPass

Preconditions: active session; model present (else `FAILED_PRECONDITION`
— the GUI always calls `EnsureModel` first); no pass running.

- Runs the local vision LLM over every image and asks whether it is a
  real-world photograph. The prompt and model are the back end's
  concern; the GUI only consumes the results.
- Emits `JunkFlag` for images classified as non-photographs, with a
  short `reason` (e.g. "screenshot", "scan", "meme", "non-photograph")
  and `confidence`, then `JunkComplete`.
- Must observe cancellation within ~1 s (abort in-flight inference).
- Deterministic for a given model and image.

### RunSimilarPass

Preconditions: active session; no pass running.

- Groups near-duplicate images (size ≥ 2) using perceptual similarity
  (perceptual hashing and/or color histograms; feature matching for
  borderline cases — algorithm is the back end's concern).
- For each group, selects the recommended keeper by a composite "best"
  score combining sharpness (Laplacian), color-balance/contrast, and
  face quality (e.g. eyes open) when faces are present. `member_scores`
  are these composite scores, best-first, parallel to `image_ids`.
- Emits one `SimilarGroup` per group, then `SimilarComplete`.
- Deterministic: re-running produces the same groups and scores.

### RunTripsPass

Preconditions: active session; no pass running. Re-runnable at any time
(the GUI re-clusters when the user changes thresholds).

- Considers only images with a usable `taken` timestamp; the rest are
  counted in `unassigned`.
- **Home detection.** The GPS photos are binned onto a ~11 km grid; a cell
  is a "home" when it is dense (≥ 5 photos or ≥ 5% of all GPS photos) AND
  its photos recur across the archive — spanning ≥ 7 days of wall-clock
  time and ≥ 50% of the whole timeline. Several homes are allowed
  (home, work, a relative's place). An archive with no such cell (a pure
  travel dump) has no home, so every cluster is a trip.
- **Away segmentation.** Walking in time order, a photo is *away* when it
  has GPS and lies farther than `home_radius_km` from every home (a photo
  without GPS inherits the previous photo's state). A run of away photos
  is one trip while the time gap stays `≤ max_gap_hours` AND the running
  trip centroid has not drifted past `max_distance_km` (the drift guard
  breaks clusters glued together by clock skew; ordinary travel does not
  trip it). At-home photos are bucketed by calendar month into `is_home`
  trips.
- **Legs.** Each trip is split into contiguous `TripLeg`s whenever GPS
  jumps past `leg_radius_km` from the current leg centroid (Rome → Florence
  → Venice; a revisit is a fresh leg).
- **Naming.** When the bundled GeoNames table (`backend/data/cities.tsv`)
  is present, each leg centroid is reverse-geocoded to "City, Country" and
  the trip `folder` becomes e.g. `"Rome, Italy · April 2026"`, or
  `"Italy · April 2026 (Rome, Florence, …)"` for a multi-leg trip;
  `folder_slug` is the filesystem-safe form used by the commit layout.
  Without the table, `folder` falls back to `"Month Year"`.
- Emits one `Trip` per cluster (ids in chronological order, with start,
  end, centroid/legs/folder/place_name when derivable), then
  `TripsComplete`.
- Metadata-only work: must complete in well under a second for a
  50,000-image session; `TripsProgress` is emitted for uniformity.

### Commit

Preconditions: active session; no pass running.

- Creates `destination` (and any missing parents) if absent.
- Copies each `keep_id`'s file into `destination`. By default the path
  relative to the session folder is preserved (subdirectories included).
  When `folder_for_id` maps the id to a sub-path, the file instead lands
  at `destination/<sub-path>/<original filename>` (the trip/leg folder
  layout). Sub-paths are sanitised: an absolute path or one containing a
  `..` component is ignored and the file falls back to the preserved
  layout.
- Collision policy: if the destination file already exists — same size:
  treated as already copied (counts toward `copied`, making re-commits
  idempotent). Different size in the preserved layout: not overwritten,
  counted in `skipped`, reported in `errors` as `"<id>: name conflict"`.
  Different size in the `folder_for_id` layout: written under a `-<n>`
  suffix, since two distinct files legitimately share a name in one trip
  folder.
- Copy failures (permissions, disk full, source disappeared) are
  reported per-file in `errors` and do not abort the run.
- Emits `CommitProgress` per file, then `CommitComplete`.
- Never modifies, moves, or deletes anything in the session folder.

## 6. Code Generation

- **C++ (back end):** Bazel `proto_library` + `cpp_grpc_library` from
  `rules_proto_grpc_cpp` (one rule generates both the protobuf message code
  and the gRPC service code), wired in `proto/BUILD.bazel`.
- **Dart (front end):** `protoc` with `protoc_plugin`
  (`dart pub global activate protoc_plugin`; `~/.pub-cache/bin` on `PATH`),
  output under `frontend/lib/src/generated/kustavi/` (gitignored; regenerate
  with `just proto`). A single `--dart_out="grpc:..."` invocation generates
  both the message and the gRPC client code. The proto file is compiled from
  a `kustavi/` root so the generated paths carry the package name:

  ```sh
  mkdir -p frontend/lib/src/generated && \
  tmp="$(mktemp -d)" && \
  mkdir "$tmp/kustavi" && \
  cp proto/service.proto "$tmp/kustavi/" && \
  protoc -I "$tmp" --dart_out="grpc:frontend/lib/src/generated" \
    kustavi/service.proto && \
  rm -rf "$tmp"
  ```

  Imported in Dart as
  `package:kustavi/src/generated/kustavi/service.pb.dart` /
  `service.pbgrpc.dart`. A `just proto` recipe runs this.
- The proto file is the contract: any change must be accompanied by
  updates to `spec/proto.md` and regeneration of the generated code on
  both sides (Bazel for C++, `just proto` for Dart).
- Field numbers are stable for the lifetime of the project; if a field
  is ever removed, its number must be `reserved`.

## 7. Versioning

The GUI and back end are released as a single versioned pair and run
together; there is no cross-version compatibility requirement.
`GetInfoResponse.version` exists for diagnostics (shown in the GUI's
back-end log dialog) and for asserting version skew in tests.
