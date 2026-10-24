# Back-End Specification

## 1. Overview

The back end is the `kustavi_engine` library (`backend/`). The GUI links it and calls it in its own
process. The calls and their messages are specified in `spec/proto.md`; this document describes
how the library is built and what its passes do.

- `backend/include/engine.h` declares the `engine` class, one method per call, and the event types
  that a pass queues for its caller. `backend/include/status.h` has `status`, `request_context`
  and `event_writer`.
- `backend/src/service/` implements the calls, one file per pass. `backend/src/pass/` holds the
  image processing that the passes use, with no knowledge of the calls. `backend/src/store/` is
  the session database (embedded SQLite, `.kustavi-cache/session.db` under the source folder).
- The engine serves one folder at a time. Persistence is per folder: re-opening a folder resumes
  its session, with its pass results and the user's decisions.
- The back end never moves, modifies or deletes files inside the source folder. `Commit` only
  copies.

## 2. Threading

- A streaming call runs on the thread that calls it. It starts a producer thread for the pass
  and drains the producer's `event_queue` into the caller's `event_writer`. The caller cancels the
  call through its `request_context`, and the call stops the producer through a stop token.
- The session database is open in serialized mode, because the unary calls and a pass use it
  from different threads.
- At most one pass runs at a time. `ensure_model` is exempt, and a mutex keeps two downloads
  from running together.
- A pass catches its exceptions and ends with an `internal` status. The library does not let an
  exception reach the caller.

## 3. Data Files

The place table (`cities.tsv`) and the face model (`face_detection_yunet.onnx`) are found beside the
program, in a `data` folder beside it, or through `KUSTAVI_GEO_DATA` and `KUSTAVI_FACE_MODEL`
(`backend/include/paths.h`). The vision model is downloaded to the user's application data folder.

## 4. Tests

- Unit tests for the passes are small programs in `backend/test/`, each built from the sources it
  checks (label `backend`).
- `engine_test` runs every call against a scratch copy of `test/photos`: the scan, the quality,
  similar, repair and trips passes, commit with its estimate and merge, resume, batch scoping,
  one pass at a time and cancelling. Its junk test downloads the vision model, so it runs only
  under `ctest -L junk` (`just test-junk`).
- The `sanitize` preset builds the library and these tests with ASan and UBSan.

## Pass Details

### ScanFolder
Starts or intercepts an active session. If an existing `session.db` with matching workspace state layout parameters is parsed, and `force_fresh` is false, it reads the entry inventory totals instantly and returns `resumed_session = true` along with the high-watermark `saved_wizard_phase` index payload to the caller, bypassing redundant calculations.
If `force_fresh` is true, or no database asset is found, it initializes the SQLite scheme, walks the system paths recursively, generates downscaled 768px image artifacts to `.kustavi-cache/res768/`, and indexes metadata into the file engine table blocks.

### Saved decisions
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

### RunRepairPass
Precondition: Active session. Repairs dates and GPS before the trips pass:
dates fall back from EXIF to the file name to the file modification time;
cameras whose clock disagrees with a GPS-tagged reference camera (a phone) get
a proposed minute offset, applied only when the client sends it back; photos
without GPS borrow the position of a photo with GPS taken within a few
minutes. Originals are kept in the session index so each run starts clean. See
`spec/proto.md` §RunRepairPass.

### Commit
Creates target destination path layouts. Copies each kept item together with its related files: same-stem RAW and HEIC originals, Live Photo `.mov` clips (which the scan does not list as separate items), and `.xmp`/`.aae` sidecars sitting adjacent within the original folders. Companions share the item's final name, so a `-<n>` collision suffix applies to the whole group. By default each file keeps its path relative to the session folder; when `CommitRequest.folder_for_id` maps its id to a sub-path, the file is placed under `destination/<sub-path>/` (the trip/leg folder layout) instead, with `-<n>` suffixing on same-folder name collisions. Sub-paths that are absolute or contain `..` are ignored. When `CommitRequest.merge_existing` is set, items already present byte-for-byte under the destination are skipped (`already_present`) and new items reuse existing destination folders. `EstimateCommit` reports the bytes to write and the free space at the destination, and `Commit` refuses to start with `FAILED_PRECONDITION` when they do not fit. Tracks progress using byte counts (`done_bytes` and `total_bytes`) alongside item counters for linear rendering representation.
