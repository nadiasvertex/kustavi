import 'dart:async';
import 'dart:collection';

import 'package:grpc/grpc.dart';
import 'package:path/path.dart' as p;
import 'package:riverpod_annotation/riverpod_annotation.dart';

import '../backend/client.dart' show KustaviClient, mapToBackendError;
import '../backend/client_provider.dart';
import '../generated/kustavi/service.pb.dart' as pb;
import 'decisions.dart';
import 'domain.dart';
import 'model_status.dart';
import 'phases.dart';

part 'wizard.g.dart';

/// The wizard controller (spec/frontend.md §6, §9).
///
/// Owns the incremental image index (the GUI's single source of truth for
/// image metadata, §5) and the linear phase machine. One pass stream is in
/// flight at a time; `EnsureModel` is exempt (it runs in [ModelStatus]).
@Riverpod(keepAlive: true)
class Wizard extends _$Wizard {
  final Map<String, ImageInfo> _images = {};
  final List<String> _orderedIds = [];
  final Map<String, QualityFlagInfo> _qualityFlags = {};
  final Map<String, JunkFlagInfo> _junkFlags = {};
  final List<SimilarGroupInfo> _similarGroups = [];
  final Map<String, VideoFlagInfo> _videoFlags = {};
  int _videoTotal = 0;

  // --- batches ---------------------------------------------------------------
  //
  // After the trips pass the photos are split into batches (one per output
  // folder, plus the unassigned photos). The user then runs whichever passes
  // they like on one batch at a time from the batch menu.

  /// Key of the batch holding photos that belong to no trip.
  static const String kUnassignedBatchKey = '__unassigned__';

  /// True from the end of the Organize stage: the batch menu and its pass
  /// runs/reviews are active.
  bool _batchMode = false;
  final List<BatchInfo> _batches = [];

  /// The batch whose passes the menu shows.
  String? _selectedBatchKey;

  /// The batch a running or open pass applies to; null outside a batch run.
  String? _activeBatchKey;

  /// Passes finished per batch, as `"<WizardStep index>:<batch key>"` (the
  /// same form the back end reports in `completed_batch_passes`).
  final Set<String> _batchPassDone = {};

  /// Passes a restored session finished over the whole library, by
  /// [WizardStep] index; they count as done for every batch.
  final Set<int> _sessionPassDone = {};

  // Junk-pass timing profile: the vision model's per-image cost is unknown
  // until measured on this machine. Profiling starts at the first progress
  // event that follows a real inference gap (resume bursts for already-
  // classified images arrive back-to-back and are skipped).
  DateTime? _junkProfileStart;
  int? _junkProfileBaseDone;
  DateTime? _junkLastEventAt;
  int _junkLastDone = 0;
  final List<TripInfo> _tripResults = [];
  final Map<int, String> _tripFolderNames = {};

  /// Per-image trip reassignment applied on top of the clustering result.
  /// The value is a trip id, or [_kUnassignedTrip] for "pulled out of every
  /// trip". Cleared whenever the trips pass re-runs.
  final Map<String, int> _tripMembership = {};

  /// Trips the user created by hand; their members live in [_tripMembership].
  final List<TripInfo> _userTrips = [];
  int _nextUserTripId = 1000000;

  /// Whether the commit step should lay files out in trip/leg folders.
  bool _organizeIntoTripFolders = true;

  /// Trips-pass tunables (GUI sliders; defaults match the back end).
  int _tripGapHours = 48;
  int _tripDistanceKm = 300;
  int _tripHomeRadiusKm = 15;
  int _tripLegRadiusKm = 25;

  static const int _kUnassignedTrip = -1;

  // Quality pass thresholds (user-adjustable, defaults match back end)
  static const double _kDefaultBlurThreshold = 100.0;
  static const double _kDefaultUnderexposedThreshold = 0.3;
  static const double _kDefaultOverexposedThreshold = 0.3;

  double _blurThreshold = _kDefaultBlurThreshold;
  double _underexposedThreshold = _kDefaultUnderexposedThreshold;
  double _overexposedThreshold = _kDefaultOverexposedThreshold;

  /// Whether the quality pass has been run at least once (so we have
  /// last-run thresholds to compare against).
  bool _hasLastRunThresholds = false;
  double _lastBlurThreshold = _kDefaultBlurThreshold;
  double _lastUnderexposedThreshold = _kDefaultUnderexposedThreshold;
  double _lastOverexposedThreshold = _kDefaultOverexposedThreshold;

  // Public accessors for the UI (quality review screen)
  double get blurThreshold => _blurThreshold;
  double get underexposedThreshold => _underexposedThreshold;
  double get overexposedThreshold => _overexposedThreshold;

  StreamSubscription<dynamic>? _passSubscription;
  bool _cancelRequested = false;
  pb.ScanComplete? _pendingScanComplete;
  WizardPhase? _returnPhase;

  /// Set while a resume re-runs the trips pass: the saved [WizardStep] index
  /// to land on afterwards. Null during normal operation. While set, progress
  /// write-through is suppressed.
  int? _resumeTargetStep;

  /// True from a resume ScanFolder until [_onResumeScanDone] finishes wiring
  /// the restored state — suppresses progress/decision write-through.
  bool _resuming = false;

  /// True while [DeletionPlan.hydrate] runs, so the decision-plan listener
  /// does not echo the restored state straight back to the back end.
  bool _hydratingDecisions = false;

  /// The scanned source folder; the commit step suggests a `<name>-kept`
  /// sibling of it as the default destination.
  String _sourceFolder = '';

  /// Commit-step state. `_commitDestination` is the user-editable field value
  /// ('' → use the suggested default). The rest are captured when the run
  /// starts / completes so the S12 progress and S13 summary can render.
  String _commitDestination = '';
  List<String> _commitKeepIds = const <String>[];
  int _commitTotalBytes = 0;
  String _committedDestination = '';
  int _commitCopied = 0;
  int _commitSkipped = 0;
  List<String> _commitErrors = const <String>[];

  Map<String, ImageInfo> get images => UnmodifiableMapView(_images);

  /// True once the Organize stage is done and passes run per batch.
  bool get batchMode => _batchMode;

  BatchInfo? _batchByKey(String? key) {
    if (key == null) {
      return null;
    }
    for (final batch in _batches) {
      if (batch.key == key) {
        return batch;
      }
    }
    return null;
  }

  /// Ids of the batch a pass is running or being reviewed for, or null when no
  /// batch is active (reviews then show everything).
  Set<String>? get reviewScope {
    return _batchByKey(_activeBatchKey)?.imageIds.toSet();
  }

  /// Scope sent to the back end for the active batch (empty = whole session).
  List<String> get _scopeIds =>
      _batchByKey(_activeBatchKey)?.imageIds ?? const <String>[];

  /// Back-end `batch_key` for the active batch ('' = none).
  String get _activeBatchKeyArg => _activeBatchKey ?? '';

  /// Similar groups with a member in the active batch (all groups when no
  /// batch is active).
  List<SimilarGroupInfo> get reviewSimilarGroups {
    final scope = reviewScope;
    if (scope == null) {
      return similarGroups;
    }
    return _similarGroups
        .where((group) => group.memberIds.any(scope.contains))
        .toList(growable: false);
  }

  /// Image ids in scan (walk) order.
  List<String> get imageIds => List<String>.unmodifiable(_orderedIds);

  List<ImageInfo> get orderedImages =>
      _orderedIds.map((id) => _images[id]!).toList(growable: false);

  Map<String, QualityFlagInfo> get qualityFlags =>
      UnmodifiableMapView(_qualityFlags);

  Map<String, JunkFlagInfo> get junkFlags => UnmodifiableMapView(_junkFlags);

  List<SimilarGroupInfo> get similarGroups =>
      List<SimilarGroupInfo>.unmodifiable(_similarGroups);

  Map<String, VideoFlagInfo> get videoFlags => UnmodifiableMapView(_videoFlags);

  /// Effective trips: the clustering result plus hand-created trips, with
  /// per-image reassignments applied, sorted chronologically. Empty trips
  /// (all members moved away) are dropped.
  List<TripInfo> get tripResults {
    final deleted = _deletedImageIds();
    final trips = <TripInfo>[];
    for (final base in [..._tripResults, ..._userTrips]) {
      final ids = _effectiveMemberIds(base, deleted);
      if (ids.isEmpty) {
        continue;
      }
      trips.add(_rebuildTrip(base, ids));
    }
    trips.sort((a, b) => a.start.compareTo(b.start));
    return List<TripInfo>.unmodifiable(trips);
  }

  bool get organizeIntoTripFolders => _organizeIntoTripFolders;
  set organizeIntoTripFolders(bool value) {
    _organizeIntoTripFolders = value;
    _publishTripsReviewPhase();
  }

  int get tripGapHours => _tripGapHours;
  int get tripDistanceKm => _tripDistanceKm;
  int get tripHomeRadiusKm => _tripHomeRadiusKm;
  int get tripLegRadiusKm => _tripLegRadiusKm;

  List<String> _effectiveMemberIds(TripInfo base, Set<String> deleted) {
    final ids = <String>{};
    for (final id in base.memberIds) {
      if (deleted.contains(id)) {
        continue;
      }
      if ((_tripMembership[id] ?? base.id) == base.id) {
        ids.add(id);
      }
    }
    _tripMembership.forEach((id, tid) {
      if (tid == base.id && !deleted.contains(id)) {
        ids.add(id);
      }
    });
    final ordered = ids.toList()
      ..sort((a, b) {
        final ta = _images[a]?.taken;
        final tb = _images[b]?.taken;
        if (ta != null && tb != null && ta != tb) {
          return ta.compareTo(tb);
        }
        return a.compareTo(b);
      });
    return ordered;
  }

  TripInfo _rebuildTrip(TripInfo base, List<String> ids) {
    final idSet = ids.toSet();
    // Untouched trip: keep the back end's start/end/centroid/legs verbatim.
    if (idSet.length == base.memberIds.length &&
        idSet.containsAll(base.memberIds)) {
      return base;
    }
    DateTime? first;
    DateTime? last;
    double latSum = 0;
    double lonSum = 0;
    int gps = 0;
    for (final id in ids) {
      final img = _images[id];
      if (img == null) {
        continue;
      }
      final taken = img.taken;
      if (taken != null) {
        if (first == null || taken.isBefore(first)) first = taken;
        if (last == null || taken.isAfter(last)) last = taken;
      }
      final g = img.gps;
      if (g != null) {
        latSum += g.$1;
        lonSum += g.$2;
        gps++;
      }
    }
    final centroid = gps > 0 ? (latSum / gps, lonSum / gps) : null;

    // Members were only removed (photos marked for deletion, or pulled out of
    // the trip): keep the back end's leg split and its geocoded place names,
    // just dropping the removed ids from each leg.
    final onlyRemovals = base.memberIds.toSet().containsAll(idSet);
    if (onlyRemovals && base.legs.length > 1) {
      final legs = <TripLegInfo>[];
      for (final leg in base.legs) {
        final kept = leg.memberIds
            .where(idSet.contains)
            .toList(growable: false);
        if (kept.isEmpty) {
          continue;
        }
        legs.add(
          TripLegInfo(
            placeName: leg.placeName,
            slug: leg.slug,
            memberIds: List<String>.unmodifiable(kept),
            centroid: leg.centroid,
          ),
        );
      }
      return base.copyWith(
        start: first ?? base.start,
        end: last ?? base.end,
        memberIds: List<String>.unmodifiable(ids),
        centroid: centroid,
        legs: legs,
      );
    }

    return base.copyWith(
      start: first ?? base.start,
      end: last ?? base.end,
      memberIds: List<String>.unmodifiable(ids),
      centroid: centroid,
      // Hand edits invalidate the back end's leg split; collapse to one leg.
      legs: <TripLegInfo>[
        TripLegInfo(
          placeName: base.placeName,
          slug: base.folderSlug.isNotEmpty ? base.folderSlug : 'leg-1',
          memberIds: List<String>.unmodifiable(ids),
          centroid: centroid,
        ),
      ],
    );
  }

  /// Effective folder name for a trip: user-renamed, or auto-generated.
  String _effectiveFolderOf(TripInfo trip) {
    return _tripFolderNames[trip.id] ?? (trip.folder ?? '');
  }

  /// Trips grouped into named folders, sorted by folder name then by start date.
  List<TripFolderInfo> get tripFolders {
    final Map<String, List<TripInfo>> byFolder = {};
    for (final trip in tripResults) {
      final name = _effectiveFolderOf(trip);
      byFolder.putIfAbsent(name, () => []).add(trip);
    }
    final sortedNames = List<String>.from(byFolder.keys)..sort();
    return sortedNames
        .map((name) => TripFolderInfo(name: name, trips: byFolder[name]!))
        .toList(growable: false);
  }

  /// Image ids not in any effective trip and not already marked for deletion
  /// (never-clustered photos plus any the user pulled out of a trip).
  List<String> get unassignedTripImageIds {
    final deleted = _deletedImageIds();
    final assigned = <String>{};
    for (final trip in tripResults) {
      assigned.addAll(trip.memberIds);
    }
    return _orderedIds
        .where((id) => !assigned.contains(id) && !deleted.contains(id))
        .toList(growable: false);
  }

  /// Reassigns [imageIds] to [tripId] (null → pull them out of every trip).
  void moveImagesToTrip(Iterable<String> imageIds, int? tripId) {
    for (final id in imageIds) {
      _tripMembership[id] = tripId ?? _kUnassignedTrip;
    }
    _publishTripsReviewPhase();
  }

  /// Creates a new trip seeded with [imageIds]; returns its id.
  int createTripFromImages(Iterable<String> imageIds) {
    final ids = imageIds.toList(growable: false);
    final id = _nextUserTripId++;
    DateTime? first;
    for (final imgId in ids) {
      final taken = _images[imgId]?.taken;
      if (taken != null && (first == null || taken.isBefore(first))) {
        first = taken;
      }
    }
    final anchor = first ?? DateTime.now();

    // The front end has no geo table; borrow a place name from the back end's
    // original clustering when the seed photos came from a geocoded trip/leg.
    final place = _dominantKnownPlace(ids);
    final String folder;
    if (place != null) {
      folder = '$place · ${_monthYear(anchor)}';
    } else if (first != null) {
      folder = 'Trip · ${_monthYear(anchor)}';
    } else {
      folder = 'New trip';
    }

    _userTrips.add(
      TripInfo(
        id: id,
        start: anchor,
        end: anchor,
        memberIds: const <String>[],
        folder: folder,
        placeName: place ?? '',
      ),
    );
    for (final imgId in ids) {
      _tripMembership[imgId] = id;
    }
    _publishTripsReviewPhase();
    return id;
  }

  /// The place name most of [imageIds] were originally clustered under (the
  /// leg's when known, else the trip's), or null when none were geocoded.
  String? _dominantKnownPlace(List<String> imageIds) {
    final counts = <String, int>{};
    for (final imgId in imageIds) {
      final place = _knownPlaceOf(imgId);
      if (place != null) {
        counts.update(place, (n) => n + 1, ifAbsent: () => 1);
      }
    }
    if (counts.isEmpty) {
      return null;
    }
    return counts.entries.reduce((a, b) => b.value > a.value ? b : a).key;
  }

  /// The geocoded place [imageId] was clustered under by the trips pass: its
  /// leg's place when the trip has legs, otherwise the trip's. Empty when the
  /// pass ran without a place table or the photo was never clustered.
  String? _knownPlaceOf(String imageId) {
    for (final trip in _tripResults) {
      for (final leg in trip.legs) {
        if (leg.placeName.isNotEmpty && leg.memberIds.contains(imageId)) {
          return leg.placeName;
        }
      }
      if (trip.placeName.isNotEmpty && trip.memberIds.contains(imageId)) {
        return trip.placeName;
      }
    }
    return null;
  }

  /// Per-image destination sub-path for `CommitRequest.folderForId`, built
  /// from the effective trip/leg layout. Empty when the user opted out.
  Map<String, String> commitFolderPlan() {
    if (!_organizeIntoTripFolders) {
      return const <String, String>{};
    }
    final plan = <String, String>{};
    for (final trip in tripResults) {
      // Prefer the back end's geocoded slug ("rome-italy-2026-04"); only fall
      // back to slugifying the display label when the user renamed the folder
      // or the trips pass had no place table.
      final renamed = _tripFolderNames.containsKey(trip.id);
      final tripSlug = renamed
          ? _slugify(_tripFolderNames[trip.id]!)
          : (trip.folderSlug.isNotEmpty
                ? trip.folderSlug
                : _slugify(_effectiveFolderOf(trip)));
      if (tripSlug.isEmpty) {
        continue;
      }
      if (trip.legs.length > 1) {
        for (final leg in trip.legs) {
          final legSlug = leg.slug.isNotEmpty ? _slugify(leg.slug) : 'leg';
          for (final id in leg.memberIds) {
            plan[id] = '$tripSlug/$legSlug';
          }
        }
      }
      for (final id in trip.memberIds) {
        plan.putIfAbsent(id, () => tripSlug);
      }
    }
    return plan;
  }

  static String _monthYear(DateTime d) {
    const months = [
      'January',
      'February',
      'March',
      'April',
      'May',
      'June',
      'July',
      'August',
      'September',
      'October',
      'November',
      'December',
    ];
    return '${months[d.month - 1]} ${d.year}';
  }

  static String _slugify(String text) {
    final buffer = StringBuffer();
    var pendingSep = false;
    for (final rune in text.toLowerCase().runes) {
      final isAlnum =
          (rune >= 0x30 && rune <= 0x39) || (rune >= 0x61 && rune <= 0x7a);
      if (isAlnum) {
        if (pendingSep && buffer.isNotEmpty) buffer.write('-');
        pendingSep = false;
        buffer.writeCharCode(rune);
      } else {
        pendingSep = true;
      }
    }
    return buffer.toString();
  }

  /// Renames the folder that [tripId] belongs to to [newName].
  void renameTripFolder(int tripId, String newName) {
    if (newName.isEmpty) {
      return;
    }
    _tripFolderNames[tripId] = newName;
    _publishTripsReviewPhase();
  }

  /// Re-runs the trips pass with updated slider values, discarding any
  /// hand edits (they are defined against the previous clustering).
  void rerunTripsPass({
    int? gapHours,
    int? distanceKm,
    int? homeRadiusKm,
    int? legRadiusKm,
  }) {
    if (state.value is! WizardTripsReview) {
      return;
    }
    _tripGapHours = gapHours ?? _tripGapHours;
    _tripDistanceKm = distanceKm ?? _tripDistanceKm;
    _tripHomeRadiusKm = homeRadiusKm ?? _tripHomeRadiusKm;
    _tripLegRadiusKm = legRadiusKm ?? _tripLegRadiusKm;
    _resetTripEdits();
    _tripResults.clear();
    state = const AsyncValue.data(WizardTripsRunning());
    final client = ref.read(kustaviClientProvider).requireValue;
    _subscribe(
      client.runTripsPass(_tripsRequest()),
      _onTripsEvent,
      _onTripsDone,
    );
  }

  void _resetTripEdits() {
    _tripMembership.clear();
    _userTrips.clear();
    _tripFolderNames.clear();
    _nextUserTripId = 1000000;
  }

  void _publishTripsReviewPhase() {
    if (state.value is WizardTripsReview) {
      state = AsyncValue.data(_tripsReviewPhase);
    }
  }

  int _scopeCount() => reviewScope?.length ?? _images.length;

  int _inScopeCount(Iterable<String> ids) {
    final scope = reviewScope;
    return scope == null ? ids.length : ids.where(scope.contains).length;
  }

  WizardQualityReview get _qualityReviewPhase => WizardQualityReview(
    flaggedCount: _inScopeCount(_qualityFlags.keys),
    totalImages: _scopeCount(),
    rerunEnabled: _hasThresholdChanges,
    previewFlagged: _previewFlagged,
  );

  /// Flagged count the stored metrics give at the current slider values.
  int? _previewFlagged;
  int _previewSeq = 0;

  /// Asks the back end how many photos the current sliders would flag. Only
  /// the newest request may publish, so a slow answer for an earlier slider
  /// position cannot overwrite a later one.
  Future<void> _refreshQualityPreview() async {
    final seq = ++_previewSeq;
    final client = ref.read(kustaviClientProvider).value;
    if (client == null) {
      return;
    }
    try {
      final response = await client.previewQualityThresholds(
        blurThreshold: _blurThreshold,
        underexposedThreshold: _underexposedThreshold,
        overexposedThreshold: _overexposedThreshold,
        scopeImageIds: _scopeIds,
      );
      if (seq != _previewSeq) {
        return;
      }
      _previewFlagged = response.flagged;
      _publishQualityReviewPhase();
    } on Object {
      // The preview is advisory; leave the previous count in place.
    }
  }

  bool get _hasThresholdChanges {
    if (!_hasLastRunThresholds) {
      return false;
    }
    return _blurThreshold != _lastBlurThreshold ||
        _underexposedThreshold != _lastUnderexposedThreshold ||
        _overexposedThreshold != _lastOverexposedThreshold;
  }

  WizardJunkReview get _junkReviewPhase => WizardJunkReview(
    flaggedCount: _inScopeCount(_junkFlags.keys),
    totalImages: _scopeCount(),
  );

  @override
  FutureOr<WizardPhase> build() async {
    // S5: the moment the model becomes ready while the user waits on the
    // junk preparation screen, start the junk pass automatically.
    ref.listen(modelStatusProvider, (previous, next) {
      if (state.value is WizardJunkPrep && next.value is ModelPrepReady) {
        _startJunkPass();
      }
    });

    // Persist the wizard's position + tunables whenever it settles on a
    // resting screen, so a later launch can offer to resume here.
    listenSelf((previous, next) {
      if (_resuming || _resumeTargetStep != null) {
        return;
      }
      final phase = next.value;
      if (phase != null && _isRestingPhase(phase)) {
        _persistProgress(phase);
      }
    });

    // Mirror the user's keep/delete choices to the back end. Each call is a
    // small idempotent "replace" against a local process, so it is fine to
    // fire on every toggle without debouncing.
    ref.listen(deletionPlanProvider, (previous, next) {
      if (_hydratingDecisions) {
        return;
      }
      _persistDecisions(next);
    });

    return const WizardStart();
  }

  static bool _isRestingPhase(WizardPhase phase) =>
      phase is WizardConfirmFolder ||
      phase is WizardQualityReview ||
      phase is WizardSimilarReview ||
      phase is WizardJunkReview ||
      phase is WizardVideoReview ||
      phase is WizardTripsReview ||
      phase is WizardBatchMenu ||
      phase is WizardCommitSummary;

  void _persistProgress(WizardPhase phase) {
    final client = ref.read(kustaviClientProvider).value;
    if (client == null) {
      return;
    }
    final request = pb.SaveSessionStateRequest()
      ..step = phase.stepIndex
      ..qualityThresholds = (pb.RunQualityPassRequest()
        ..blurThreshold = _blurThreshold
        ..underexposedThreshold = _underexposedThreshold
        ..overexposedThreshold = _overexposedThreshold)
      ..tripParams = _tripsRequest();
    unawaited(_safeSave(client, request));
  }

  void _persistDecisions(DeletionIntent plan) {
    final client = ref.read(kustaviClientProvider).value;
    if (client == null) {
      return;
    }
    final phase = state.value;
    if (phase == null ||
        phase is WizardStart ||
        phase is WizardScanning ||
        phase is WizardSessionRestore) {
      return;
    }
    final request = pb.SaveSessionStateRequest()..replaceDecisions = true;
    for (final id in plan.explicitKept) {
      request.decisions.add(
        pb.DecisionEntry()
          ..imageId = id
          ..decision = pb.Decision.KEEP,
      );
    }
    for (final id in plan.explicitDeleted) {
      request.decisions.add(
        pb.DecisionEntry()
          ..imageId = id
          ..decision = pb.Decision.DELETE,
      );
    }
    plan.groupKeepers.forEach((groupId, keeperId) {
      request.groupKeepers[groupId] = keeperId;
    });
    unawaited(_safeSave(client, request));
  }

  Future<void> _safeSave(
    KustaviClient client,
    pb.SaveSessionStateRequest request,
  ) async {
    try {
      await client.saveSessionState(request);
    } on Object {
      // Progress persistence is best-effort; a failure must not disrupt the
      // wizard. The next resting screen will try again.
    }
  }

  // --- S0 -> S1 ----------------------------------------------------------

  void selectFolder(String folder) {
    if (state.value is! WizardStart) {
      return;
    }
    _clearPassResults();
    _returnPhase = null;
    _resumeTargetStep = null;
    _resuming = false;
    _sourceFolder = folder;
    final client = ref.read(kustaviClientProvider);
    if (client case AsyncData<KustaviClient>(:final value)) {
      unawaited(_beginFromFolder(value, folder));
    } else if (client case AsyncError(:final error, :final stackTrace)) {
      state = AsyncValue.error(error, stackTrace);
    } else {
      state = AsyncValue.error(
        BackendRpc(
          const GrpcError.unavailable(
            'Back end is still starting up. Please wait a moment.',
          ),
        ),
        StackTrace.current,
      );
    }
  }

  /// Probe the folder for saved progress before scanning: a resumable session
  /// routes to [WizardSessionRestore]; otherwise a fresh scan starts.
  Future<void> _beginFromFolder(KustaviClient client, String folder) async {
    pb.InspectSessionResponse probe;
    try {
      probe = await client.inspectSession(folder);
    } on Object {
      probe = pb.InspectSessionResponse(); // treat a probe failure as "fresh"
    }
    if (state.value is! WizardStart) {
      return; // the user navigated away while the probe was in flight
    }
    if (probe.hasSession && probe.imageCount > 0) {
      state = AsyncValue.data(
        WizardSessionRestore(
          folder: folder,
          imageCount: probe.imageCount,
          savedStepIndex: probe.resumeStep,
        ),
      );
    } else {
      _startFreshScan(client, folder);
    }
  }

  void _startFreshScan(KustaviClient client, String folder) {
    state = AsyncValue.data(WizardScanning(folder: folder));
    final request = pb.ScanFolderRequest()
      ..folder = folder
      ..recursive = true
      ..resume = false;
    _subscribe(client.scanFolder(request), _onScanEvent, _onScanDone);
  }

  // --- S0-B: resume a saved session ------------------------------------------

  /// [WizardSessionRestore] "Resume": re-emit the saved index, then rehydrate
  /// results/decisions and re-enter the pipeline at the saved step.
  void resumeSession() {
    if (state.value is! WizardSessionRestore) {
      return;
    }
    final folder = (state.value as WizardSessionRestore).folder;
    _clearPassResults();
    _sourceFolder = folder;
    _resuming = true;
    state = AsyncValue.data(WizardScanning(folder: folder));
    final client = ref.read(kustaviClientProvider).requireValue;
    final request = pb.ScanFolderRequest()
      ..folder = folder
      ..recursive = true
      ..resume = true;
    _subscribe(client.scanFolder(request), _onScanEvent, _onResumeScanDone);
  }

  /// [WizardSessionRestore] "Start fresh": discard saved progress and scan.
  void startFreshFromRestore() {
    if (state.value is! WizardSessionRestore) {
      return;
    }
    final folder = (state.value as WizardSessionRestore).folder;
    _clearPassResults();
    _returnPhase = null;
    _resumeTargetStep = null;
    _resuming = false;
    _sourceFolder = folder;
    final client = ref.read(kustaviClientProvider).requireValue;
    _startFreshScan(client, folder);
  }

  Future<void> _onResumeScanDone() async {
    if (state.value is! WizardScanning) {
      return;
    }
    final complete = _pendingScanComplete;
    _pendingScanComplete = null;
    if (complete == null || complete.images == 0) {
      _resuming = false;
      state = AsyncValue.data(
        WizardConfirmFolder(
          folder: _sourceFolder,
          imageCount: _orderedIds.length,
        ),
      );
      return;
    }

    final client = ref.read(kustaviClientProvider).requireValue;
    pb.GetSessionResultsResponse results;
    try {
      results = await client.getSessionResults();
    } on Object catch (error, stackTrace) {
      _resuming = false;
      state = AsyncValue.error(
        error is BackendError ? error : mapToBackendError(error),
        stackTrace,
      );
      return;
    }

    // Restore tunables (proto3 zeroes unset fields -> keep the defaults).
    final t = results.qualityThresholds;
    if (t.blurThreshold > 0) _blurThreshold = t.blurThreshold;
    if (t.underexposedThreshold > 0) {
      _underexposedThreshold = t.underexposedThreshold;
    }
    if (t.overexposedThreshold > 0) {
      _overexposedThreshold = t.overexposedThreshold;
    }
    final p = results.tripParams;
    if (p.maxGapHours > 0) _tripGapHours = p.maxGapHours;
    if (p.maxDistanceKm > 0) _tripDistanceKm = p.maxDistanceKm;
    if (p.homeRadiusKm > 0) _tripHomeRadiusKm = p.homeRadiusKm;
    if (p.legRadiusKm > 0) _tripLegRadiusKm = p.legRadiusKm;
    _saveLastRunThresholds();

    // Restore every persisted pass result. Nothing here re-runs; the ladder
    // below only runs a pass that never finished (or was never reached).
    _qualityFlags.clear();
    for (final flag in results.qualityFlags) {
      _qualityFlags[flag.imageId] = QualityFlagInfo.fromFlag(flag);
    }
    _similarGroups.clear();
    for (final group in results.similarGroups) {
      _similarGroups.add(SimilarGroupInfo.fromGroup(group));
    }
    _junkFlags.clear();
    for (final flag in results.junkFlags) {
      _junkFlags[flag.imageId] = JunkFlagInfo.fromFlag(flag);
    }
    _videoFlags.clear();
    for (final flag in results.videoFlags) {
      _videoFlags[flag.videoId] = VideoFlagInfo.fromFlag(flag);
    }
    _videoTotal = results.videoTotal;

    // Restore the user's keep/delete choices.
    final kept = <String>{};
    final deleted = <String>{};
    for (final entry in results.decisions) {
      if (entry.decision == pb.Decision.DELETE) {
        deleted.add(entry.imageId);
      } else if (entry.decision == pb.Decision.KEEP) {
        kept.add(entry.imageId);
      }
    }
    final keepers = <int, String>{};
    results.groupKeepers.forEach((groupId, keeperId) {
      keepers[groupId] = keeperId;
    });
    _hydratingDecisions = true;
    ref
        .read(deletionPlanProvider.notifier)
        .hydrate(kept: kept, deleted: deleted, keepers: keepers);
    _hydratingDecisions = false;

    _resuming = false;

    final target = complete.resumeStep;
    if (target < WizardStep.quality.index) {
      state = AsyncValue.data(
        WizardConfirmFolder(
          folder: _sourceFolder,
          imageCount: _orderedIds.length,
        ),
      );
      return;
    }

    // Pass results are restored above. Batches come from the trips pass,
    // which is cheap and has no persisted result, so it runs again; the
    // user then lands on the batch menu (or the commit summary).
    _resumeTargetStep = target;
    _sessionPassDone
      ..clear()
      ..addAll([
        if (results.qualityDone) WizardStep.quality.index,
        if (results.similarDone) WizardStep.duplicates.index,
        if (results.junkDone) WizardStep.junk.index,
        if (results.videoDone) WizardStep.video.index,
      ]);
    _batchPassDone
      ..clear()
      ..addAll(results.completedBatchPasses);
    _returnPhase = WizardConfirmFolder(
      folder: _sourceFolder,
      imageCount: _orderedIds.length,
    );
    _startTripsPass();
  }

  void cancelScan() {
    if (state.value is! WizardScanning) {
      return;
    }
    _cancelPass();
    state = const AsyncValue.data(WizardStart());
  }

  void _onScanEvent(pb.ScanEvent event) {
    final phase = state.value;
    if (phase is! WizardScanning) {
      return;
    }
    switch (event.whichEvent()) {
      case pb.ScanEvent_Event.progress:
        state = AsyncValue.data(
          phase.copyWith(
            filesSeen: event.progress.filesSeen,
            imagesFound: event.progress.imagesFound,
            currentPath: event.progress.currentPath,
          ),
        );
      case pb.ScanEvent_Event.image:
        final meta = event.image;
        if (!_images.containsKey(meta.id)) {
          _orderedIds.add(meta.id);
        }
        _images[meta.id] = ImageInfo.fromMeta(meta);
        state = AsyncValue.data(
          phase.copyWith(
            imagesFound: _orderedIds.length,
            currentPath: meta.name,
          ),
        );
      case pb.ScanEvent_Event.complete:
        _pendingScanComplete = event.complete;
      case pb.ScanEvent_Event.notSet:
        break;
    }
  }

  void _onScanDone() {
    if (state.value is! WizardScanning) {
      return;
    }
    final folder = (state.value as WizardScanning).folder;
    final complete = _pendingScanComplete;
    _pendingScanComplete = null;
    if (complete == null) {
      return;
    }
    if (complete.images == 0) {
      state = AsyncValue.data(WizardNoImages(folder: folder));
    } else {
      // Fresh scan: a saved session is detected earlier (in [selectFolder] via
      // InspectSession) and handled by [resumeSession] / [_onResumeScanDone].
      state = AsyncValue.data(
        WizardConfirmFolder(
          folder: folder,
          imageCount: _orderedIds.length,
          scanErrors: complete.errors,
        ),
      );
    }
  }

  // --- S2 ----------------------------------------------------------------

  void backFromConfirm() {
    if (state.value is! WizardConfirmFolder) {
      return;
    }
    _clearPassResults();
    state = const AsyncValue.data(WizardStart());
  }

  /// Confirm -> Organize: the trips pass runs first, because its folders
  /// become the batches the user reviews.
  void continueFromConfirm() {
    if (state.value is! WizardConfirmFolder) {
      return;
    }
    _returnPhase = state.value;
    _startTripsPass();
  }

  void _startTripsPass() {
    _tripResults.clear();
    _resetTripEdits();
    state = const AsyncValue.data(WizardTripsRunning());
    final client = ref.read(kustaviClientProvider).requireValue;
    _subscribe(
      client.runTripsPass(_tripsRequest()),
      _onTripsEvent,
      _onTripsDone,
    );
  }

  // --- S3 ----------------------------------------------------------------

  void _onQualityEvent(pb.QualityEvent event) {
    if (state.value is! WizardQualityRunning) {
      return;
    }
    switch (event.whichEvent()) {
      case pb.QualityEvent_Event.progress:
        state = AsyncValue.data(
          WizardQualityRunning(
            done: event.progress.done,
            total: event.progress.total,
          ),
        );
      case pb.QualityEvent_Event.flag:
        final flag = QualityFlagInfo.fromFlag(event.flag);
        _qualityFlags[flag.imageId] = flag;
      case pb.QualityEvent_Event.complete:
        break;
      case pb.QualityEvent_Event.notSet:
        break;
    }
  }

  void _onQualityDone() {
    if (state.value is! WizardQualityRunning) {
      return;
    }
    _saveLastRunThresholds();
    _previewFlagged = null;
    _finishBatchRun(WizardStep.quality, () => _qualityReviewPhase);
  }

  void _saveLastRunThresholds() {
    _hasLastRunThresholds = true;
    _lastBlurThreshold = _blurThreshold;
    _lastUnderexposedThreshold = _underexposedThreshold;
    _lastOverexposedThreshold = _overexposedThreshold;
  }

  void cancelQuality() {
    if (state.value is! WizardQualityRunning) {
      return;
    }
    _cancelPass();
    _returnToBatchMenu();
  }

  // --- quality rerun --------------------------------------------------------

  void rerunQualityPass() {
    if (state.value is! WizardQualityReview || !_hasThresholdChanges) {
      return;
    }
    _startQualityPass();
  }

  /// Runs the quality pass over the active batch with the current sliders.
  /// Its flags are recomputed, so the batch's old ones are dropped first.
  void _startQualityPass() {
    final scope = reviewScope;
    _qualityFlags.removeWhere((id, _) => scope == null || scope.contains(id));
    _saveLastRunThresholds();
    state = const AsyncValue.data(WizardQualityRunning());
    final client = ref.read(kustaviClientProvider).requireValue;
    _subscribe(
      client.runQualityPass(
        blurThreshold: _blurThreshold,
        underexposedThreshold: _underexposedThreshold,
        overexposedThreshold: _overexposedThreshold,
        scopeImageIds: _scopeIds,
        batchKey: _activeBatchKeyArg,
      ),
      _onQualityEvent,
      _onQualityDone,
    );
  }

  void setBlurThreshold(double value) {
    if (value == _blurThreshold) {
      return;
    }
    _blurThreshold = value;
    _publishQualityReviewPhase();
    unawaited(_refreshQualityPreview());
  }

  void setUnderexposedThreshold(double value) {
    if (value == _underexposedThreshold) {
      return;
    }
    _underexposedThreshold = value;
    _publishQualityReviewPhase();
    unawaited(_refreshQualityPreview());
  }

  void setOverexposedThreshold(double value) {
    if (value == _overexposedThreshold) {
      return;
    }
    _overexposedThreshold = value;
    _publishQualityReviewPhase();
    unawaited(_refreshQualityPreview());
  }

  void resetThresholds() {
    if (_blurThreshold == _kDefaultBlurThreshold &&
        _underexposedThreshold == _kDefaultUnderexposedThreshold &&
        _overexposedThreshold == _kDefaultOverexposedThreshold) {
      return;
    }
    _blurThreshold = _kDefaultBlurThreshold;
    _underexposedThreshold = _kDefaultUnderexposedThreshold;
    _overexposedThreshold = _kDefaultOverexposedThreshold;
    _publishQualityReviewPhase();
    unawaited(_refreshQualityPreview());
  }

  /// Republishes the quality review phase with the current threshold state
  /// (recomputing [WizardQualityReview.rerunEnabled]). [WizardPhase] is
  /// immutable and Riverpod only notifies listeners when the state value
  /// differs, so each changed threshold must produce a *new* phase instance
  /// — reassigning the same instance is a silent no-op and the review
  /// screen would never rebuild.
  void _publishQualityReviewPhase() {
    if (state.value is WizardQualityReview) {
      state = AsyncValue.data(_qualityReviewPhase);
    }
  }

  // --- batch menu -------------------------------------------------------------

  /// Builds the batches from the effective trip folders (plus the photos in no
  /// trip) and opens the batch menu.
  void _enterBatchMode() {
    _batches.clear();
    for (final folder in tripFolders) {
      final ids = <String>{};
      for (final trip in folder.trips) {
        ids.addAll(trip.memberIds);
      }
      if (ids.isEmpty) {
        continue;
      }
      _batches.add(
        BatchInfo(
          key: folder.name.isEmpty ? '__no-folder__' : folder.name,
          title: folder.name.isEmpty ? 'Other' : folder.name,
          imageIds: _orderedIds.where(ids.contains).toList(growable: false),
        ),
      );
    }
    final unassigned = unassignedTripImageIds;
    if (unassigned.isNotEmpty) {
      _batches.add(
        BatchInfo(
          key: kUnassignedBatchKey,
          title: 'Unassigned',
          imageIds: unassigned,
        ),
      );
    }
    _batchMode = true;
    _activeBatchKey = null;
    _runAllQueue.clear();
    _runAllStep = null;
    _selectedBatchKey = _batches.isEmpty ? null : _batches.first.key;
  }

  WizardBatchMenu get _batchMenuPhase {
    return WizardBatchMenu(
      batches: [for (final batch in _batches) _summarize(batch)],
      selectedKey: _selectedBatchKey,
      markedCount: _deletedImageIds().length,
      totalImages: _images.length,
    );
  }

  bool _passDone(WizardStep step, String batchKey) =>
      _sessionPassDone.contains(step.index) ||
      _batchPassDone.contains('${step.index}:$batchKey');

  BatchSummary _summarize(BatchInfo batch) {
    final ids = batch.imageIds.toSet();
    var videos = 0;
    for (final id in ids) {
      if (_images[id]?.isVideo ?? false) {
        videos++;
      }
    }
    int flagged(Iterable<String> flaggedIds) =>
        flaggedIds.where(ids.contains).length;
    BatchPassStatus status(WizardStep step, int flaggedCount) =>
        BatchPassStatus(
          done: _passDone(step, batch.key),
          flagged: flaggedCount,
        );
    return BatchSummary(
      key: batch.key,
      title: batch.title,
      photoCount: ids.length - videos,
      videoCount: videos,
      passes: {
        WizardStep.quality.index: status(
          WizardStep.quality,
          flagged(_qualityFlags.keys),
        ),
        WizardStep.duplicates.index: status(
          WizardStep.duplicates,
          _similarGroups
              .where((group) => group.memberIds.any(ids.contains))
              .length,
        ),
        WizardStep.junk.index: status(
          WizardStep.junk,
          flagged(_junkFlags.keys),
        ),
        WizardStep.video.index: status(
          WizardStep.video,
          flagged(_videoFlags.keys),
        ),
      },
    );
  }

  void selectBatch(String key) {
    if (state.value is! WizardBatchMenu || _batchByKey(key) == null) {
      return;
    }
    _selectedBatchKey = key;
    state = AsyncValue.data(_batchMenuPhase);
  }

  /// Back to the menu from a pass or review, dropping the active batch.
  void _returnToBatchMenu() {
    _activeBatchKey = null;
    _runAllQueue.clear();
    _runAllStep = null;
    if (_batchMode) {
      state = AsyncValue.data(_batchMenuPhase);
    } else {
      state = AsyncValue.data(_returnPhase ?? const WizardStart());
    }
  }

  /// Runs [step] on the selected batch; its review opens when it finishes.
  void startBatchPass(WizardStep step) {
    if (state.value is! WizardBatchMenu || _selectedBatchKey == null) {
      return;
    }
    _runAllQueue.clear();
    _runAllStep = null;
    _activeBatchKey = _selectedBatchKey;
    _beginPass(step);
  }

  /// Runs [step] on every batch that has not finished it, one after another,
  /// then returns to the menu.
  void startPassOnAllBatches(WizardStep step) {
    if (state.value is! WizardBatchMenu) {
      return;
    }
    final pending = [
      for (final batch in _batches)
        if (!_passDone(step, batch.key)) batch.key,
    ];
    if (pending.isEmpty) {
      return;
    }
    _runAllStep = step;
    _runAllQueue
      ..clear()
      ..addAll(pending.skip(1));
    _activeBatchKey = pending.first;
    _beginPass(step);
  }

  final List<String> _runAllQueue = [];
  WizardStep? _runAllStep;

  /// Opens the review for a pass the selected batch already finished.
  void reviewBatchPass(WizardStep step) {
    if (state.value is! WizardBatchMenu || _selectedBatchKey == null) {
      return;
    }
    _activeBatchKey = _selectedBatchKey;
    state = AsyncValue.data(switch (step) {
      WizardStep.quality => _qualityReviewPhase,
      WizardStep.duplicates => _similarReviewPhase,
      WizardStep.junk => _junkReviewPhase,
      _ => _videoReviewPhase,
    });
  }

  void _beginPass(WizardStep step) {
    final client = ref.read(kustaviClientProvider).requireValue;
    switch (step) {
      case WizardStep.quality:
        _startQualityPass();
      case WizardStep.duplicates:
        // Groups are recomputed, so the batch's old ones are dropped.
        final scope = reviewScope;
        _similarGroups.removeWhere(
          (group) => scope == null || group.memberIds.any(scope.contains),
        );
        state = const AsyncValue.data(WizardSimilarRunning());
        _subscribe(
          client.runSimilarPass(
            skipImageIds: _deletedBeforeSimilar(),
            scopeImageIds: _scopeIds,
            batchKey: _activeBatchKeyArg,
          ),
          _onSimilarEvent,
          _onSimilarDone,
        );
      case WizardStep.junk:
        // Images the pass already classified are not re-emitted, so existing
        // flags are kept.
        if (_modelReady) {
          _startJunkPass();
        } else {
          state = const AsyncValue.data(WizardJunkPrep());
        }
      case WizardStep.video:
        state = const AsyncValue.data(WizardVideoRunning());
        _subscribe(
          client.runVideoPass(
            skipVideoIds: _deletedBeforeVideo(),
            scopeImageIds: _scopeIds,
            batchKey: _activeBatchKeyArg,
          ),
          _onVideoEvent,
          _onVideoDone,
        );
      case WizardStep.select || WizardStep.trips || WizardStep.copy:
        break;
    }
  }

  /// A pass finished for the active batch: record it, then either start the
  /// next queued batch, return to the menu (end of a run-all), or open the
  /// pass's review.
  void _finishBatchRun(WizardStep step, WizardPhase Function() review) {
    final key = _activeBatchKey;
    if (key != null) {
      _batchPassDone.add('${step.index}:$key');
    }
    if (_runAllStep == step) {
      if (_runAllQueue.isNotEmpty) {
        _activeBatchKey = _runAllQueue.removeAt(0);
        _beginPass(step);
      } else {
        _returnToBatchMenu();
      }
      return;
    }
    state = AsyncValue.data(review());
  }

  /// [Done] on any batch review: back to the menu.
  void closeBatchReview() {
    final phase = state.value;
    if (phase is WizardQualityReview ||
        phase is WizardSimilarReview ||
        phase is WizardJunkReview ||
        phase is WizardVideoReview) {
      _returnToBatchMenu();
    }
  }

  /// Menu -> commit summary.
  void continueFromBatches() {
    if (state.value is! WizardBatchMenu) {
      return;
    }
    _activeBatchKey = null;
    state = AsyncValue.data(_commitSummaryPhase);
  }

  /// Menu -> back to the trip folders to regroup. Batch progress marks are
  /// dropped because the batches are rebuilt from the edited folders.
  void reopenOrganize() {
    if (state.value is! WizardBatchMenu) {
      return;
    }
    _batchMode = false;
    _batchPassDone.clear();
    _sessionPassDone.clear();
    _activeBatchKey = null;
    state = AsyncValue.data(_tripsReviewPhase);
  }

  void keepAllQualityFlagged() {
    if (state.value is! WizardQualityReview) {
      return;
    }
    ref
        .read(deletionPlanProvider.notifier)
        .keepAll(_flaggedInScope(_qualityFlags.keys));
  }

  void markAllQualityFlagged() {
    if (state.value is! WizardQualityReview) {
      return;
    }
    ref
        .read(deletionPlanProvider.notifier)
        .markAll(_flaggedInScope(_qualityFlags.keys));
  }

  /// [ids] limited to the active batch (all of them when none is active).
  List<String> _flaggedInScope(Iterable<String> ids) {
    final scope = reviewScope;
    return scope == null
        ? ids.toList(growable: false)
        : ids.where(scope.contains).toList(growable: false);
  }

  bool get _modelReady {
    return ref.read(modelStatusProvider).value is ModelPrepReady;
  }

  /// Ids marked for deletion by the quality step, so the duplicate pass never
  /// scores them or picks them as a group keeper.
  List<String> _deletedBeforeSimilar() {
    final plan = ref.read(deletionPlanProvider);
    final qualityFlagged = _qualityFlags.keys.toSet();
    bool marked(String id) => isMarkedForDeletion(
      plan,
      id,
      step: DeletionStep.quality,
      qualityFlagged: qualityFlagged,
      junkFlagged: const <String>{},
      similarKeepers: const <String, String>{},
    );
    return _images.keys.where(marked).toList(growable: false);
  }

  /// Ids already marked for deletion by an earlier step, so the video pass
  /// can skip analysis on them.
  List<String> _deletedBeforeVideo() {
    final plan = ref.read(deletionPlanProvider);
    final keepers = similarKeeperMap(plan, _similarGroups);
    final qualityFlagged = _qualityFlags.keys.toSet();
    final junkFlagged = _junkFlags.keys.toSet();
    bool marked(String id) => DeletionStep.values
        .where((step) => step != DeletionStep.video)
        .any(
          (step) => isMarkedForDeletion(
            plan,
            id,
            step: step,
            qualityFlagged: qualityFlagged,
            junkFlagged: junkFlagged,
            similarKeepers: keepers,
          ),
        );
    return _images.keys.where(marked).toList(growable: false);
  }

  /// Organize -> batch menu: the folders as they stand now become the batches.
  void continueFromTrips() {
    if (state.value is! WizardTripsReview) {
      return;
    }
    _enterBatchMode();
    state = AsyncValue.data(_batchMenuPhase);
  }

  void cancelTrips() {
    if (state.value is! WizardTripsRunning) {
      return;
    }
    _cancelPass();
    state = AsyncValue.data(_returnPhase ?? const WizardStart());
  }

  pb.RunTripsPassRequest _tripsRequest() {
    return pb.RunTripsPassRequest()
      ..maxGapHours = _tripGapHours
      ..maxDistanceKm = _tripDistanceKm
      ..homeRadiusKm = _tripHomeRadiusKm
      ..legRadiusKm = _tripLegRadiusKm;
  }

  void cancelJunkPrep() {
    if (state.value is! WizardJunkPrep) {
      return;
    }
    ref.read(modelStatusProvider.notifier).cancelDownload();
    _returnToBatchMenu();
  }

  void _startJunkPass() {
    _junkProfileStart = null;
    _junkProfileBaseDone = null;
    _junkLastEventAt = null;
    _junkLastDone = 0;
    state = const AsyncValue.data(WizardJunkRunning());
    final client = ref.read(kustaviClientProvider).requireValue;
    _subscribe(
      client.runJunkPass(
        skipImageIds: _deletedBeforeJunk(),
        scopeImageIds: _scopeIds,
        batchKey: _activeBatchKeyArg,
      ),
      _onJunkEvent,
      _onJunkDone,
    );
  }

  /// Ids already marked for deletion by the quality or duplicates step, so
  /// the junk pass can skip inference on them.
  List<String> _deletedBeforeJunk() {
    final plan = ref.read(deletionPlanProvider);
    final keepers = similarKeeperMap(plan, _similarGroups);
    final qualityFlagged = _qualityFlags.keys.toSet();
    bool marked(String id) =>
        isMarkedForDeletion(
          plan,
          id,
          step: DeletionStep.quality,
          qualityFlagged: qualityFlagged,
          junkFlagged: const <String>{},
          similarKeepers: keepers,
        ) ||
        isMarkedForDeletion(
          plan,
          id,
          step: DeletionStep.similar,
          qualityFlagged: qualityFlagged,
          junkFlagged: const <String>{},
          similarKeepers: keepers,
        );
    return _images.keys.where(marked).toList(growable: false);
  }

  WizardSimilarReview get _similarReviewPhase => WizardSimilarReview(
    groupCount: reviewSimilarGroups.length,
    markedCount: _similarMarkedCount(),
  );

  WizardTripsReview get _tripsReviewPhase {
    final trips = tripResults;
    return WizardTripsReview(
      tripCount: trips.length,
      tripFolders: tripFolders,
      markedCount: _tripsMarkedCount(),
      trips: trips,
      unassignedCount: unassignedTripImageIds.length,
    );
  }

  /// Re-publishes S10 after the deletion plan changed under it (e.g. a photo
  /// toggled for deletion from the detail view).
  void refreshTripsReview() => _publishTripsReviewPhase();

  /// Image ids marked for deletion by any step (quality, junk, similar) or
  /// explicitly by the user. These are excluded from the trips panel and from
  /// the commit copy set.
  Set<String> _deletedImageIds() {
    final plan = ref.read(deletionPlanProvider);
    final keepers = similarKeeperMap(plan, _similarGroups);
    final qualityFlagged = _qualityFlags.keys.toSet();
    final junkFlagged = _junkFlags.keys.toSet();
    final videoFlagged = _videoFlags.keys.toSet();
    bool deleted(String id) => DeletionStep.values.any(
      (step) => isMarkedForDeletion(
        plan,
        id,
        step: step,
        qualityFlagged: qualityFlagged,
        junkFlagged: junkFlagged,
        similarKeepers: keepers,
        videoFlagged: videoFlagged,
      ),
    );
    return _images.keys.where(deleted).toSet();
  }

  /// Image ids to copy at commit: every scanned image not marked for deletion
  /// by any step, in scan order.
  List<String> _keepIds() {
    final deleted = _deletedImageIds();
    return _orderedIds
        .where((id) => !deleted.contains(id))
        .toList(growable: false);
  }

  /// The suggested default destination: a sibling of the source folder named
  /// `<source-name>-kept` (spec/frontend.md §6.2 S11, §15).
  String _suggestedDestination() {
    if (_sourceFolder.isEmpty) {
      return '';
    }
    final normalized = p.normalize(_sourceFolder);
    final parent = p.dirname(normalized);
    final name = p.basename(normalized);
    if (name.isEmpty) {
      return '';
    }
    return p.join(parent, '$name-kept');
  }

  /// The destination that a commit would use: the user's field value, or the
  /// suggested default when they have not typed one.
  String get _effectiveCommitDestination => _commitDestination.isNotEmpty
      ? _commitDestination
      : _suggestedDestination();

  WizardCommitSummary get _commitSummaryPhase {
    final keepIds = _keepIds();
    var keepBytes = 0;
    for (final id in keepIds) {
      keepBytes += _images[id]?.sizeBytes ?? 0;
    }
    return WizardCommitSummary(
      keepCount: keepIds.length,
      keepBytes: keepBytes,
      leftBehindCount: _images.length - keepIds.length,
      destination: _effectiveCommitDestination,
    );
  }

  /// Photos the back end clustered into a trip that the user has since marked
  /// for deletion (by any step). Shown as the S10 "marked" stat.
  int _tripsMarkedCount() {
    final deleted = _deletedImageIds();
    if (deleted.isEmpty) {
      return 0;
    }
    final clustered = <String>{};
    for (final base in _tripResults) {
      clustered.addAll(base.memberIds);
    }
    return clustered.intersection(deleted).length;
  }

  void _onSimilarDone() {
    if (state.value is! WizardSimilarRunning) {
      return;
    }
    _finishBatchRun(WizardStep.duplicates, () => _similarReviewPhase);
  }

  // --- Trips pass ---------------------------------------------------------

  void _onTripsEvent(pb.TripsEvent event) {
    if (state.value is! WizardTripsRunning) {
      return;
    }
    switch (event.whichEvent()) {
      case pb.TripsEvent_Event.progress:
        state = AsyncValue.data(
          WizardTripsRunning(
            done: event.progress.done,
            total: event.progress.total,
          ),
        );
      case pb.TripsEvent_Event.trip:
        _tripResults.add(TripInfo.fromTrip(event.trip));
      case pb.TripsEvent_Event.complete:
        break;
      case pb.TripsEvent_Event.notSet:
        break;
    }
  }

  void _onTripsDone() {
    if (state.value is! WizardTripsRunning) {
      return;
    }
    final target = _resumeTargetStep;
    if (target != null) {
      // A resumed session re-runs the (cheap) trips pass, then lands where it
      // left off: the commit summary, or the batch menu for anything earlier.
      _resumeTargetStep = null;
      _enterBatchMode();
      state = AsyncValue.data(
        target == WizardStep.copy.index ? _commitSummaryPhase : _batchMenuPhase,
      );
      return;
    }
    state = AsyncValue.data(_tripsReviewPhase);
  }

  // --- S6 ----------------------------------------------------------------

  void _onJunkEvent(pb.JunkEvent event) {
    if (state.value is! WizardJunkRunning) {
      return;
    }
    switch (event.whichEvent()) {
      case pb.JunkEvent_Event.progress:
        final done = event.progress.done;
        final total = event.progress.total;
        final now = DateTime.now();

        // Begin (or extend) the profile once an event follows a real gap —
        // i.e. the vision model actually spent time on an image.
        if (_junkLastEventAt != null &&
            now.difference(_junkLastEventAt!).inMilliseconds >= 250) {
          _junkProfileStart ??= _junkLastEventAt;
          _junkProfileBaseDone ??= _junkLastDone;
        }
        _junkLastEventAt = now;
        _junkLastDone = done;

        double? secondsPerImage;
        DateTime? estimatedCompletion;
        if (_junkProfileStart != null && _junkProfileBaseDone != null) {
          final measured = done - _junkProfileBaseDone!;
          final elapsedMs = now.difference(_junkProfileStart!).inMilliseconds;
          if (measured > 0 && elapsedMs > 0) {
            secondsPerImage = elapsedMs / 1000 / measured;
            final remaining = total - done;
            estimatedCompletion = remaining > 0
                ? now.add(
                    Duration(
                      milliseconds: (secondsPerImage * remaining * 1000)
                          .round(),
                    ),
                  )
                : now;
          }
        }

        state = AsyncValue.data(
          WizardJunkRunning(
            done: done,
            total: total,
            secondsPerImage: secondsPerImage,
            estimatedCompletion: estimatedCompletion,
          ),
        );
      case pb.JunkEvent_Event.flag:
        final flag = JunkFlagInfo.fromFlag(event.flag);
        _junkFlags[flag.imageId] = flag;
      case pb.JunkEvent_Event.complete:
        break;
      case pb.JunkEvent_Event.notSet:
        break;
    }
  }

  void _onJunkDone() {
    if (state.value is! WizardJunkRunning) {
      return;
    }
    _finishBatchRun(WizardStep.junk, () => _junkReviewPhase);
  }

  void cancelJunk() {
    if (state.value is! WizardJunkRunning) {
      return;
    }
    _cancelPass();
    _returnToBatchMenu();
  }

  // --- S7 ----------------------------------------------------------------

  void keepAllJunkFlagged() {
    if (state.value is! WizardJunkReview) {
      return;
    }
    ref
        .read(deletionPlanProvider.notifier)
        .keepAll(_flaggedInScope(_junkFlags.keys));
  }

  void markAllJunkFlagged() {
    if (state.value is! WizardJunkReview) {
      return;
    }
    ref
        .read(deletionPlanProvider.notifier)
        .markAll(_flaggedInScope(_junkFlags.keys));
  }

  // --- S8 ----------------------------------------------------------------

  void _onSimilarEvent(pb.SimilarEvent event) {
    if (state.value is! WizardSimilarRunning) {
      return;
    }
    switch (event.whichEvent()) {
      case pb.SimilarEvent_Event.progress:
        state = AsyncValue.data(
          WizardSimilarRunning(
            done: event.progress.done,
            total: event.progress.total,
          ),
        );
      case pb.SimilarEvent_Event.group:
        _similarGroups.add(SimilarGroupInfo.fromGroup(event.group));
      case pb.SimilarEvent_Event.complete:
        break;
      case pb.SimilarEvent_Event.notSet:
        break;
    }
  }

  int _similarMarkedCount() {
    final plan = ref.read(deletionPlanProvider);
    final keepers = similarKeeperMap(plan, _similarGroups);
    return reviewSimilarGroups
        .expand((group) => group.memberIds)
        .where(
          (id) => isMarkedForDeletion(
            plan,
            id,
            step: DeletionStep.similar,
            qualityFlagged: _qualityFlags.keys.toSet(),
            junkFlagged: _junkFlags.keys.toSet(),
            similarKeepers: keepers,
          ),
        )
        .length;
  }

  void cancelSimilar() {
    if (state.value is! WizardSimilarRunning) {
      return;
    }
    _cancelPass();
    _returnToBatchMenu();
  }

  /// Organize [Back]: to the folder confirmation.
  void backFromTrips() {
    if (state.value is! WizardTripsReview) {
      return;
    }
    state = AsyncValue.data(_returnPhase ?? const WizardStart());
  }

  // --- S10-B/C: video pass -----------------------------------------------

  void _onVideoEvent(pb.VideoEvent event) {
    if (state.value is! WizardVideoRunning) {
      return;
    }
    switch (event.whichEvent()) {
      case pb.VideoEvent_Event.progress:
        _videoTotal = event.progress.total;
        state = AsyncValue.data(
          WizardVideoRunning(
            done: event.progress.done,
            total: event.progress.total,
          ),
        );
      case pb.VideoEvent_Event.flag:
        final flag = VideoFlagInfo.fromFlag(event.flag);
        _videoFlags[flag.videoId] = flag;
      case pb.VideoEvent_Event.complete:
        break;
      case pb.VideoEvent_Event.notSet:
        break;
    }
  }

  void _onVideoDone() {
    if (state.value is! WizardVideoRunning) {
      return;
    }
    _finishBatchRun(WizardStep.video, () => _videoReviewPhase);
  }

  void cancelVideo() {
    if (state.value is! WizardVideoRunning) {
      return;
    }
    _cancelPass();
    _returnToBatchMenu();
  }

  WizardVideoReview get _videoReviewPhase {
    final scope = reviewScope;
    final totalVideos = scope == null
        ? _videoTotal
        : scope.where((id) => _images[id]?.isVideo ?? false).length;
    return WizardVideoReview(
      flaggedCount: _inScopeCount(_videoFlags.keys),
      totalVideos: totalVideos,
    );
  }

  void keepAllVideoFlagged() {
    if (state.value is! WizardVideoReview) {
      return;
    }
    ref
        .read(deletionPlanProvider.notifier)
        .keepAll(_flaggedInScope(_videoFlags.keys));
  }

  void markAllVideoFlagged() {
    if (state.value is! WizardVideoReview) {
      return;
    }
    ref
        .read(deletionPlanProvider.notifier)
        .markAll(_flaggedInScope(_videoFlags.keys));
  }

  // --- S11–S13: commit -------------------------------------------------------

  /// S11 destination field edit. Republishes the summary so the shell's
  /// [Copy] button re-evaluates its enabled state (see the note on
  /// [_publishQualityReviewPhase] for why a fresh instance is required).
  void setCommitDestination(String value) {
    if (state.value is! WizardCommitSummary || value == _commitDestination) {
      return;
    }
    _commitDestination = value;
    state = AsyncValue.data(_commitSummaryPhase);
  }

  /// S11 [Back] -> trips review.
  void backFromCommitSummary() {
    if (state.value is! WizardCommitSummary) {
      return;
    }
    if (_batchMode) {
      state = AsyncValue.data(_batchMenuPhase);
    } else {
      state = AsyncValue.data(_returnPhase ?? _tripsReviewPhase);
    }
  }

  /// S11 [Copy] -> run the commit pass (S12).
  void startCommit() {
    if (state.value is! WizardCommitSummary) {
      return;
    }
    final destination = _effectiveCommitDestination;
    if (destination.isEmpty) {
      return;
    }
    _returnPhase = _commitSummaryPhase;
    _committedDestination = destination;
    _commitKeepIds = _keepIds();
    _commitTotalBytes = _commitKeepIds.fold<int>(
      0,
      (sum, id) => sum + (_images[id]?.sizeBytes ?? 0),
    );
    _commitCopied = 0;
    _commitSkipped = 0;
    _commitErrors = const <String>[];
    state = AsyncValue.data(
      WizardCommitting(
        total: _commitKeepIds.length,
        totalBytes: _commitTotalBytes,
      ),
    );
    final client = ref.read(kustaviClientProvider).requireValue;
    final request = pb.CommitRequest(
      destination: destination,
      keepIds: _commitKeepIds,
      folderForId: commitFolderPlan().entries,
    );
    _subscribe(client.commit(request), _onCommitEvent, _onCommitDone);
  }

  void cancelCommit() {
    if (state.value is! WizardCommitting) {
      return;
    }
    _cancelPass();
    state = AsyncValue.data(_returnPhase ?? _commitSummaryPhase);
  }

  void _onCommitEvent(pb.CommitEvent event) {
    if (state.value is! WizardCommitting) {
      return;
    }
    switch (event.whichEvent()) {
      case pb.CommitEvent_Event.progress:
        final done = event.progress.done;
        // CommitProgress carries only file counts; approximate bytes-done by
        // summing the sizes of the first `done` ids in the keep set.
        var doneBytes = 0;
        for (var i = 0; i < done && i < _commitKeepIds.length; i++) {
          doneBytes += _images[_commitKeepIds[i]]?.sizeBytes ?? 0;
        }
        state = AsyncValue.data(
          WizardCommitting(
            done: done,
            total: event.progress.total,
            currentName: event.progress.currentName,
            doneBytes: doneBytes,
            totalBytes: _commitTotalBytes,
          ),
        );
      case pb.CommitEvent_Event.complete:
        _commitCopied = event.complete.copied;
        _commitSkipped = event.complete.skipped;
        _commitErrors = List<String>.unmodifiable(event.complete.errors);
      case pb.CommitEvent_Event.notSet:
        break;
    }
  }

  void _onCommitDone() {
    if (state.value is! WizardCommitting) {
      return;
    }
    state = AsyncValue.data(
      WizardDone(
        copiedCount: _commitCopied,
        skippedCount: _commitSkipped,
        destination: _committedDestination,
        errors: _commitErrors,
      ),
    );
  }

  // --- step error (§10.2) -------------------------------------------------

  /// [Back] on the step error screen: return to the phase the failed pass
  /// was started from.
  void goBackFromError() {
    if (_batchMode && _batches.isNotEmpty) {
      // A batch pass failed: keep every result so far and return to the menu.
      _cancelPass();
      _returnToBatchMenu();
      return;
    }
    _clearPassResults(keepReturnPhase: true);
    state = AsyncValue.data(_returnPhase ?? const WizardStart());
  }

  /// Resets the wizard to S0 (S13 [Start over]); the next folder selection
  /// starts a new back-end session.
  void resetToStart() {
    _clearPassResults();
    ref.read(deletionPlanProvider.notifier).reset();
    state = const AsyncValue.data(WizardStart());
  }

  // --- plumbing -----------------------------------------------------------

  void _subscribe<T>(
    Stream<T> stream,
    void Function(T) onEvent,
    void Function() onDone,
  ) {
    _cancelRequested = false;
    _passSubscription?.cancel();
    _passSubscription = stream.listen(
      onEvent,
      onError: (Object error, StackTrace stackTrace) {
        if (_cancelRequested) {
          return;
        }
        state = AsyncValue.error(
          error is BackendError ? error : mapToBackendError(error),
          stackTrace,
        );
      },
      onDone: () {
        _passSubscription = null;
        // Riverpod 3 keeps the previous value inside an AsyncError, so a
        // phase-completion handler would see the stale phase and clobber the
        // error state. Skip it when the pass already failed.
        if (state.hasError) {
          return;
        }
        onDone();
      },
    );
  }

  void _cancelPass() {
    _cancelRequested = true;
    _passSubscription?.cancel();
    _passSubscription = null;
    _pendingScanComplete = null;
  }

  void _clearPassResults({
    bool keepReturnPhase = false,
    bool keepIndex = false,
  }) {
    _cancelPass();
    _pendingScanComplete = null;
    if (!keepIndex) {
      _images.clear();
      _orderedIds.clear();
    }
    _qualityFlags.clear();
    _junkFlags.clear();
    _videoFlags.clear();
    _videoTotal = 0;
    _similarGroups.clear();
    _batchMode = false;
    _batches.clear();
    _selectedBatchKey = null;
    _activeBatchKey = null;
    _batchPassDone.clear();
    _sessionPassDone.clear();
    _runAllQueue.clear();
    _runAllStep = null;
    _tripResults.clear();
    _resetTripEdits();
    _commitDestination = '';
    _commitKeepIds = const <String>[];
    _commitTotalBytes = 0;
    _committedDestination = '';
    _commitCopied = 0;
    _commitSkipped = 0;
    _commitErrors = const <String>[];
    _resumeTargetStep = null;
    _resuming = false;
    if (!keepReturnPhase) {
      _returnPhase = null;
    }
  }
}
