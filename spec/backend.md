# Back-End RPC Specification (gRPC)

## 1. Overview

Kustavi's front end and back end communicate over gRPC on a loopback TCP
port. This document is the normative API the front end expects (see
`spec/frontend.md`); the C++ back end is specified in `spec/backend.md`
and must implement exactly this contract.

- Single source of truth: `proto/service.proto`, package `kustavi`.
- One service: `Kustavi`. All long-running operations are
  server-streaming methods that emit `oneof`-based event messages.
- Session Persistence: Persistent per folder. The back end creates and manages an embedded SQLite3 database engine stored within a hidden folder cache (`.kustavi-cache/session.db`) under the selected source path root. 
- Calling a non-`ScanFolder` method before an active session is established is rejected with `FAILED_PRECONDITION`.

## 2. Transport & Lifecycle

- The GUI launches the back end with `--listen 127.0.0.1:0 --token <auth_token>`.
- After binding, the back end prints one flushed line to stdout: `KUSTAVI-READY <port>\n`.
- Security Mandate: The backend must read the incoming gRPC metadata call headers and enforce that every incoming connection passes the matching `<auth_token>` validation signature string. Requests failing auth validation are instantly terminated.
- HTTP/2, loopback only. No TLS, no auth over public interfaces: bound tightly to 127.0.0.1.
- Concurrency Rule: At most one analysis/processing pass runs at a time. The backend rejects a second concurrent processing pass with `FAILED_PRECONDITION`. `EnsureModel` is explicitly exempt from this restriction and can execute in parallel with folder ingestion.
- Cancellation: The backend must observe client cancellation signals within 1–2 seconds, immediately terminating any active execution, file transfers, or Moondream LLM inference pipelines, safely committing atomic transactional database states before release.
- `Shutdown` is idempotent, triggers memory cleanups, and terminates the process context.

## 3. Conventions

- **Image id** — the path of the image relative to the session folder, using `/` as separator. Stable across sessions.
- **Timestamps** — milliseconds since the Unix epoch, `int64`.
- **GPS** — decimal degrees, WGS84, `(latitude, longitude)`.
- **Optional metadata** — proto3 `optional` definitions.
- **Thumbnails/Previews** — Unified 768px downscaled JPEG working image asset tier. Generated once during ingestion and written to `.kustavi-cache/res768/` before its `ImageMeta` wire event fires.
- **Status codes** — `INVALID_ARGUMENT`, `NOT_FOUND`, `FAILED_PRECONDITION`, `INTERNAL`, `CANCELLED`.
- Source Protection: The backend never moves, modifies, or deletes files inside the source folder. `Commit` is strictly copy-only.

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
  rpc FetchSavedDecisions(FetchSavedDecisionsRequest) returns (FetchSavedDecisionsResponse);

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
  string version = 1;              
  repeated string supported_formats = 2;  
  string model_name = 3;           
}

message ShutdownRequest {}
message ShutdownResponse {}

// --- shared -------------------------------------------------------------

message GpsPoint {
  double latitude = 1;
  double longitude = 2;
}

message ImageMeta {
  string id = 1;               
  string path = 2;             
  string name = 3;             
  uint32 original_width = 4;
  uint32 original_height = 5;
  uint64 size_bytes = 6;
  optional int64 taken_unix_ms = 7;
  optional GpsPoint gps = 8;
  string working_image_path = 9;   // Absolute path to the cached 768px JPEG file
}

// --- pass 1: scan --------------------------------------------------------

message ScanFolderRequest {
  string folder = 1;           
  bool recursive = 2;          
  bool force_fresh = 3;        // True to clear existing SQLite tables and cache assets
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
  uint32 skipped_files = 2;    
  repeated string errors = 3;  
  bool resumed_session = 4;    // True if a pre-existing valid SQLite state file was detected
  uint32 saved_wizard_phase = 5; // Direct target phase step to fast-forward the frontend shell layout
}

message FetchSavedDecisionsRequest {}

message FetchSavedDecisionsResponse {
  repeated string explicit_kept_ids = 1;
  repeated string explicit_deleted_ids = 2;
  repeated string similar_keeper_reassignments = 3; // Format string array strings: "group_id:keeper_id"
}

// --- pass 2: quality -------------------------------------------------------

message RunQualityPassRequest {}

message QualityEvent {
  oneof event {
    QualityProgress progress = 1;
    QualityFlag flag = 2;      
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
  repeated QualityReason reasons = 2;  
  double sharpness = 3;      -- Standardized/normalized metrics
  double exposure_score = 4; 
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
    JunkFlag flag = 2;         
    JunkComplete complete = 3;
  }
}

message JunkProgress {
  uint32 done = 1;
  uint32 total = 2;
}

message JunkFlag {
  string image_id = 1;
  string reason = 2;    // classification: e.g. "screenshot", "scan", "meme"
  double confidence = 3;  
}

message JunkComplete {
  uint32 flagged = 1;
  uint32 total = 2;
}

// --- pass 4: similar -----------------------------------------------------------

message RunSimilarPassRequest {
  repeated string skip_image_ids = 1; // already marked for deletion upstream
}

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
  uint32 id = 1;                       
  repeated string image_ids = 2;       
  string recommended_keep_id = 3;
  repeated double member_scores = 4;   
}

message SimilarComplete {
  uint32 groups = 1;
  uint32 total_images = 2;
}

// --- pass 5: trips ----------------------------------------------------------------

message RunTripsPassRequest {
  int32 max_gap_hours = 1;
  int32 max_distance_km = 2;
  int32 home_radius_km = 3;
  int32 leg_radius_km = 4;
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
  repeated string image_ids = 4;
  optional GpsPoint centroid = 5;
  optional string folder = 6;
  string folder_slug = 7;
  string place_name = 8;
  repeated TripLeg legs = 9;
  bool is_home = 10;
}

message TripLeg {
  string place_name = 1;
  string slug = 2;
  repeated string image_ids = 3;
  optional GpsPoint centroid = 4;
}

message TripsComplete {
  uint32 trips = 1;
  uint32 unassigned = 2;  
}

// --- pass 6: commit ------------------------------------------------------------------

message CommitRequest {
  string destination = 1;
  repeated string keep_ids = 2;
  map<string, string> folder_for_id = 3;
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
  uint32 skipped = 2;        
  repeated string errors = 3;  
}
```

## 5. Method Semantics

### GetInfo
No preconditions. Returns application parameters. Used as a channel health readiness probe.

### Shutdown
Idempotent. Safely flushes transactions, finalizes SQLite handles, and quits.

### ScanFolder
Starts or intercepts an active session. If an existing `session.db` with matching workspace state layout parameters is parsed, and `force_fresh` is false, it reads the entry inventory totals instantly and returns `resumed_session = true` along with the high-watermark `saved_wizard_phase` index payload to the caller, bypassing redundant calculations.
If `force_fresh` is true, or no database asset is found, it initializes the SQLite scheme, walks the system paths recursively, generates downscaled 768px image artifacts to `.kustavi-cache/res768/`, and indexes metadata into the file engine table blocks.

### FetchSavedDecisions
Precondition: Active session populated from a recovered database layout. Pulls rows from `user_decisions` and `user_group_keepers` to restore frontend memory providers to their exact pre-exit human configuration states.

### RunQualityPass
Precondition: Active session. Processes Laplacian variance sharpness and exposure histograms on images. Saves values directly into the relational `quality_flags` table blocks.

### EnsureModel
Idempotent background pipeline. Downloads Moondream-3.1 localized weights to local system data directory structures if absent.

### RunJunkPass
Precondition: Vision LLM weights verified. Interrogates the Moondream-3.1 engine model using greedy decoding (`temperature = 0.0`) for strict session reproducibility across pauses. Leverages the pre-scaled 768px JPEGs as direct ingestion matrices to preserve memory. Skips records already tracking completed valuations inside `junk_flags`. Commits rows atomically as processing finishes. Unloads model tensors out of device memory structures if navigation exits this step.

### RunSimilarPass
Precondition: Active session. Compares assets for duplicates using perceptual indices. To scale performance efficiently to 50,000 images without running an \(O(N^2)\) brute force wall, the backend must organize comparison hashes via high-speed tree architectures (such as a **VP-Tree** or **BK-Tree**). Matches are recorded into `similar_groups`.

Images whose id is in `skip_image_ids` (marked for deletion in the quality
step) are excluded from grouping, scoring, and keeper selection; a group left
with fewer than 2 members is dropped. For each surviving group the recommended
keeper is the member with the highest composite "bestness" score: the
normalized sharpness term (Laplacian variance from the quality pass) blended
with per-image signals from `pass/keeper_signals` — gray-world color-balance
neutrality, and, when faces are present (YuNet ONNX,
`backend/data/face_detection_yunet.onnx` via `config::face_model_path()`),
largest-face focus, face count, a landmark-anchored eyes-open proxy, and a
red-eye score. The blend degrades to sharpness + color-balance when the face
model is absent. `member_scores` carries the composite score, best-first.

### RunTripsPass
Home-anchored clustering over in-memory database records. Detects recurring
"home" GPS clusters (dense cells whose photos span ≥ 7 days and ≥ 50% of the
archive timeline), then groups away-from-home photos chronologically into
trips (broken by `max_gap_hours` or a `max_distance_km` centroid-drift
guard), splits each trip into contiguous `TripLeg`s at `leg_radius_km` GPS
jumps, and reverse-geocodes leg centroids to "City, Country" folder names via
the bundled GeoNames table (`backend/data/cities.tsv`, located through
`config::geo_data_path()`; folders fall back to "Month Year" when it is
absent). At-home photos become monthly `is_home` trips. See
`spec/proto.md` §RunTripsPass for the full rule set.

### RunVideoPass
Precondition: Active session. Runs only over `images` rows with `kind = 'video'`
(ingested alongside photos during `ScanFolder` from `supported_video_extensions`,
decoded via OpenCV's `videoio` module — AVFoundation on macOS, Media Foundation
on Windows). For each video: a container that fails to open, or reports a
non-positive fps/frame count, is flagged `corrupt`; a clip shorter than
`min_duration_ms` (default 1500ms) is flagged `too_short` without decoding any
frames; otherwise `sample_frame_count` evenly spaced frames are decoded and
scored with the same Laplacian-variance sharpness function as `RunQualityPass`
(flagged `blurry` below `blur_threshold`) and the same dHash function as
`RunSimilarPass` (flagged `static` when every consecutive sampled-frame pair's
Hamming distance stays below `motion_hamming_threshold`, i.e. no motion across
the whole clip). If the vision model from `EnsureModel`/`RunJunkPass` is
already downloaded, 1-2 sampled frames are also run through that same
classifier to catch non-photographic content (e.g. screen recordings); the
vision step is skipped, not an error, when the model isn't present. Results
are recorded in `video_flags`, keyed by the video's `images.id`. Near-duplicate
and burst-recorded clips are not handled here — `RunSimilarPass` already
groups them, since video ingestion writes a representative frame to
`working_image_path` just like photos do.

### Commit
Creates target destination path layouts. Copies calculated image files while automatically grabbing matching layout sidecars (such as `.xmp` and `.aae` extensions) sitting adjacent within the original folders. By default each file keeps its path relative to the session folder; when `CommitRequest.folder_for_id` maps its id to a sub-path, the file is placed under `destination/<sub-path>/` (the trip/leg folder layout) instead, with `-<n>` suffixing on same-folder name collisions. Sub-paths that are absolute or contain `..` are ignored. Tracks progress using byte counts (`done_bytes` and `total_bytes`) alongside item counters for linear rendering representation.
