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

  // Junk-pass timing profile: the vision model's per-image cost is unknown
  // until measured on this machine. Profiling starts at the first progress
  // event that follows a real inference gap (resume bursts for already-
  // classified images arrive back-to-back and are skipped).
  DateTime? _junkProfileStart;
  int? _junkProfileBaseDone;
  DateTime? _junkLastEventAt;
  int _junkLastDone = 0;
  final List<TripInfo> _tripResults = [];
  final Map<int, Set<String>> _tripSelections = {};
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

  /// Image ids in scan (walk) order.
  List<String> get imageIds => List<String>.unmodifiable(_orderedIds);

  List<ImageInfo> get orderedImages =>
      _orderedIds.map((id) => _images[id]!).toList(growable: false);

  Map<String, QualityFlagInfo> get qualityFlags =>
      UnmodifiableMapView(_qualityFlags);

  Map<String, JunkFlagInfo> get junkFlags => UnmodifiableMapView(_junkFlags);

  List<SimilarGroupInfo> get similarGroups =>
      List<SimilarGroupInfo>.unmodifiable(_similarGroups);

  /// Effective trips: the clustering result plus hand-created trips, with
  /// per-image reassignments applied, sorted chronologically. Empty trips
  /// (all members moved away) are dropped.
  List<TripInfo> get tripResults {
    final trips = <TripInfo>[];
    for (final base in [..._tripResults, ..._userTrips]) {
      final ids = _effectiveMemberIds(base);
      if (ids.isEmpty) {
        continue;
      }
      trips.add(_rebuildTrip(base, ids));
    }
    trips.sort((a, b) => a.start.compareTo(b.start));
    return List<TripInfo>.unmodifiable(trips);
  }

  Map<int, Set<String>> get tripSelections => _tripSelections;

  bool get organizeIntoTripFolders => _organizeIntoTripFolders;
  set organizeIntoTripFolders(bool value) {
    _organizeIntoTripFolders = value;
    _publishTripsReviewPhase();
  }

  int get tripGapHours => _tripGapHours;
  int get tripDistanceKm => _tripDistanceKm;
  int get tripHomeRadiusKm => _tripHomeRadiusKm;
  int get tripLegRadiusKm => _tripLegRadiusKm;

  List<String> _effectiveMemberIds(TripInfo base) {
    final ids = <String>{};
    for (final id in base.memberIds) {
      if ((_tripMembership[id] ?? base.id) == base.id) {
        ids.add(id);
      }
    }
    _tripMembership.forEach((id, tid) {
      if (tid == base.id) {
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
    // Untouched trip: keep the back end's start/end/centroid/legs verbatim.
    if (ids.length == base.memberIds.length &&
        ids.toSet().containsAll(base.memberIds)) {
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
    return base.copyWith(
      start: first ?? base.start,
      end: last ?? base.end,
      memberIds: List<String>.unmodifiable(ids),
      centroid: gps > 0 ? (latSum / gps, lonSum / gps) : null,
      // Hand edits invalidate the back end's leg split; collapse to one leg.
      legs: <TripLegInfo>[
        TripLegInfo(
          placeName: base.placeName,
          slug: base.folderSlug.isNotEmpty ? base.folderSlug : 'leg-1',
          memberIds: List<String>.unmodifiable(ids),
          centroid: gps > 0 ? (latSum / gps, lonSum / gps) : null,
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
        .map((name) => TripFolderInfo(
              name: name,
              trips: byFolder[name]!,
            ))
        .toList(growable: false);
  }

  /// Image ids not in any effective trip and not already marked for deletion
  /// (never-clustered photos plus any the user pulled out of a trip).
  List<String> get unassignedTripImageIds {
    final assigned = <String>{};
    for (final trip in tripResults) {
      assigned.addAll(trip.memberIds);
    }
    final plan = ref.read(deletionPlanProvider);
    return _orderedIds
        .where((id) =>
            !assigned.contains(id) && !plan.explicitDeleted.contains(id))
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
    final id = _nextUserTripId++;
    DateTime? first;
    for (final imgId in imageIds) {
      final taken = _images[imgId]?.taken;
      if (taken != null && (first == null || taken.isBefore(first))) {
        first = taken;
      }
    }
    final anchor = first ?? DateTime.now();
    _userTrips.add(TripInfo(
      id: id,
      start: anchor,
      end: anchor,
      memberIds: const <String>[],
      folder: first != null ? 'Trip · ${_monthYear(anchor)}' : 'New trip',
    ));
    for (final imgId in imageIds) {
      _tripMembership[imgId] = id;
    }
    _publishTripsReviewPhase();
    return id;
  }

  /// Per-image destination sub-path for `CommitRequest.folderForId`, built
  /// from the effective trip/leg layout. Empty when the user opted out.
  Map<String, String> commitFolderPlan() {
    if (!_organizeIntoTripFolders) {
      return const <String, String>{};
    }
    final plan = <String, String>{};
    for (final trip in tripResults) {
      final folderName = _effectiveFolderOf(trip);
      final tripSlug = _slugify(
          folderName.isNotEmpty ? folderName : trip.folderSlug);
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
      'January', 'February', 'March', 'April', 'May', 'June', 'July',
      'August', 'September', 'October', 'November', 'December',
    ];
    return '${months[d.month - 1]} ${d.year}';
  }

  static String _slugify(String text) {
    final buffer = StringBuffer();
    var pendingSep = false;
    for (final rune in text.toLowerCase().runes) {
      final isAlnum = (rune >= 0x30 && rune <= 0x39) ||
          (rune >= 0x61 && rune <= 0x7a);
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
    _tripSelections.clear();
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

  WizardQualityReview get _qualityReviewPhase => WizardQualityReview(
        flaggedCount: _qualityFlags.length,
        totalImages: _images.length,
        rerunEnabled: _hasThresholdChanges,
      );

  bool get _hasThresholdChanges {
    if (!_hasLastRunThresholds) {
      return false;
    }
    return _blurThreshold != _lastBlurThreshold ||
        _underexposedThreshold != _lastUnderexposedThreshold ||
        _overexposedThreshold != _lastOverexposedThreshold;
  }

  WizardJunkReview get _junkReviewPhase => WizardJunkReview(
        flaggedCount: _junkFlags.length,
        totalImages: _images.length,
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
    return const WizardStart();
  }

  // --- S0 -> S1 ----------------------------------------------------------

  void selectFolder(String folder) {
    if (state.value is! WizardStart) {
      return;
    }
    _clearPassResults();
    _returnPhase = null;
    _sourceFolder = folder;
    state = AsyncValue.data(WizardScanning(folder: folder));
    final client = ref.read(kustaviClientProvider);
    if (client case AsyncData<KustaviClient>(:final value)) {
      final request = pb.ScanFolderRequest()..folder = folder..recursive = true;
      _subscribe(
        value.scanFolder(request),
        _onScanEvent,
        _onScanDone,
      );
    } else if (client case AsyncError(:final error, :final stackTrace)) {
      state = AsyncValue.error(error, stackTrace);
    } else {
      state = AsyncValue.error(
        BackendRpc(
          const GrpcError.unavailable('Back end is still starting up. Please wait a moment.'),
        ),
        StackTrace.current,
      );
    }
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
      // A saved session (`resumed_session`) would fast-forward to
      // WizardSessionRestore; the current wire contract does not carry the
      // field yet, so every scan is treated as a fresh session.
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

  void continueFromConfirm() {
    if (state.value is! WizardConfirmFolder) {
      return;
    }
    _returnPhase = state.value;
    _qualityFlags.clear();
    _saveLastRunThresholds();
    state = const AsyncValue.data(WizardQualityRunning());
    final client = ref.read(kustaviClientProvider).requireValue;
    _subscribe(
      client.runQualityPass(
        blurThreshold: _blurThreshold,
        underexposedThreshold: _underexposedThreshold,
        overexposedThreshold: _overexposedThreshold,
      ),
      _onQualityEvent,
      _onQualityDone,
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
    state = AsyncValue.data(_qualityReviewPhase);
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
    state = AsyncValue.data(_returnPhase ?? const WizardStart());
  }

  // --- S4 rerun -----------------------------------------------------------

  void rerunQualityPass() {
    if (state.value is! WizardQualityReview || !_hasThresholdChanges) {
      return;
    }
    // The folder did not change, so the image index is still valid — keep
    // it (the S4 total and the S2 grid both read it); only the quality
    // results are cleared, as the pass repopulates them.
    _clearPassResults(keepReturnPhase: true, keepIndex: true);
    _saveLastRunThresholds();
    state = const AsyncValue.data(WizardQualityRunning());
    final client = ref.read(kustaviClientProvider).requireValue;
    _subscribe(
      client.runQualityPass(
        blurThreshold: _blurThreshold,
        underexposedThreshold: _underexposedThreshold,
        overexposedThreshold: _overexposedThreshold,
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
  }

  void setUnderexposedThreshold(double value) {
    if (value == _underexposedThreshold) {
      return;
    }
    _underexposedThreshold = value;
    _publishQualityReviewPhase();
  }

  void setOverexposedThreshold(double value) {
    if (value == _overexposedThreshold) {
      return;
    }
    _overexposedThreshold = value;
    _publishQualityReviewPhase();
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

  // --- S4 ----------------------------------------------------------------

  void backFromQuality() {
    if (state.value is! WizardQualityReview) {
      return;
    }
    state = AsyncValue.data(_returnPhase ?? const WizardStart());
  }

  void keepAllQualityFlagged() {
    if (state.value is! WizardQualityReview) {
      return;
    }
    ref.read(deletionPlanProvider.notifier).keepAll(_qualityFlags.keys);
  }

  void markAllQualityFlagged() {
    if (state.value is! WizardQualityReview) {
      return;
    }
    ref.read(deletionPlanProvider.notifier).markAll(_qualityFlags.keys);
  }

  bool get _modelReady {
    return ref.read(modelStatusProvider).value is ModelPrepReady;
  }

  void continueFromQuality() {
    if (state.value is! WizardQualityReview) {
      return;
    }
    _returnPhase = state.value;
    _similarGroups.clear();
    state = const AsyncValue.data(WizardSimilarRunning());
    final client = ref.read(kustaviClientProvider).requireValue;
    _subscribe(
      client.runSimilarPass(skipImageIds: _deletedBeforeSimilar()),
      _onSimilarEvent,
      _onSimilarDone,
    );
  }

  /// Ids marked for deletion by the quality step, so the duplicate pass never
  /// scores them or picks them as a group keeper. Only the quality step has
  /// run at this point.
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

  /// Duplicates review -> junk pass (or its model-download prep screen).
  void continueFromSimilar() {
    if (state.value is! WizardSimilarReview) {
      return;
    }
    _returnPhase = state.value;
    _junkFlags.clear();
    if (_modelReady) {
      _startJunkPass();
    } else {
      state = const AsyncValue.data(WizardJunkPrep());
    }
  }

  /// Junk review -> trips pass.
  void continueFromJunk() {
    if (state.value is! WizardJunkReview) {
      return;
    }
    _returnPhase = state.value;
    _tripResults.clear();
    _tripSelections.clear();
    _resetTripEdits();
    state = const AsyncValue.data(WizardTripsRunning());
    final client = ref.read(kustaviClientProvider).requireValue;
    _subscribe(
      client.runTripsPass(_tripsRequest()),
      _onTripsEvent,
      _onTripsDone,
    );
  }

  /// Trips review -> commit summary (the last review step).
  void continueFromTrips() {
    if (state.value is! WizardTripsReview) {
      return;
    }
    _returnPhase = _tripsReviewPhase;
    state = AsyncValue.data(_commitSummaryPhase);
  }

  void cancelTrips() {
    if (state.value is! WizardTripsRunning) {
      return;
    }
    _cancelPass();
    state = AsyncValue.data(_returnPhase ?? _junkReviewPhase);
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
    state = AsyncValue.data(_returnPhase ?? _similarReviewPhase);
  }

  void _startJunkPass() {
    _junkProfileStart = null;
    _junkProfileBaseDone = null;
    _junkLastEventAt = null;
    _junkLastDone = 0;
    state = const AsyncValue.data(WizardJunkRunning());
    final client = ref.read(kustaviClientProvider).requireValue;
    _subscribe(
      client.runJunkPass(skipImageIds: _deletedBeforeJunk()),
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
        groupCount: _similarGroups.length,
        markedCount: _similarMarkedCount(),
      );

  WizardTripsReview get _tripsReviewPhase {
    final trips = tripResults;
    return WizardTripsReview(
      tripCount: trips.length,
      tripFolders: tripFolders,
      markedCount: _tripsMarkedCount(trips),
      trips: trips,
      unassignedCount: unassignedTripImageIds.length,
    );
  }

  /// Image ids to copy at commit: every scanned image not marked for deletion
  /// by any step, in scan order.
  List<String> _keepIds() {
    final plan = ref.read(deletionPlanProvider);
    final keepers = similarKeeperMap(plan, _similarGroups);
    final qualityFlagged = _qualityFlags.keys.toSet();
    final junkFlagged = _junkFlags.keys.toSet();
    bool deleted(String id) => DeletionStep.values.any(
          (step) => isMarkedForDeletion(
            plan,
            id,
            step: step,
            qualityFlagged: qualityFlagged,
            junkFlagged: junkFlagged,
            similarKeepers: keepers,
          ),
        );
    return _orderedIds.where((id) => !deleted(id)).toList(growable: false);
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
  String get _effectiveCommitDestination =>
      _commitDestination.isNotEmpty ? _commitDestination : _suggestedDestination();

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

  int _tripsMarkedCount(List<TripInfo> trips) {
    int count = 0;
    for (final trip in trips) {
      final selections = _tripSelections[trip.id];
      if (selections != null) {
        count += selections.length;
      }
    }
    return count;
  }

  void _onSimilarDone() {
    if (state.value is! WizardSimilarRunning) {
      return;
    }
    state = AsyncValue.data(_similarReviewPhase);
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
                ? now.add(Duration(
                    milliseconds: (secondsPerImage * remaining * 1000).round()))
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
    state = AsyncValue.data(_junkReviewPhase);
  }

  void cancelJunk() {
    if (state.value is! WizardJunkRunning) {
      return;
    }
    _cancelPass();
    state = AsyncValue.data(_returnPhase ?? _similarReviewPhase);
  }

  // --- S7 ----------------------------------------------------------------

  void backFromJunk() {
    if (state.value is! WizardJunkReview) {
      return;
    }
    state = AsyncValue.data(_returnPhase ?? _similarReviewPhase);
  }

  void keepAllJunkFlagged() {
    if (state.value is! WizardJunkReview) {
      return;
    }
    ref.read(deletionPlanProvider.notifier).keepAll(_junkFlags.keys);
  }

  void markAllJunkFlagged() {
    if (state.value is! WizardJunkReview) {
      return;
    }
    ref.read(deletionPlanProvider.notifier).markAll(_junkFlags.keys);
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
    return _similarGroups
        .expand((group) => group.memberIds)
        .where(
          (id) =>
              isMarkedForDeletion(
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
    state = AsyncValue.data(_returnPhase ?? _qualityReviewPhase);
  }

  // --- S9 ----------------------------------------------------------------

  void backFromSimilar() {
    if (state.value is! WizardSimilarReview) {
      return;
    }
    state = AsyncValue.data(_returnPhase ?? _qualityReviewPhase);
  }

  void backFromTrips() {
    if (state.value is! WizardTripsReview) {
      return;
    }
    state = AsyncValue.data(_returnPhase ?? _junkReviewPhase);
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
    state = AsyncValue.data(_returnPhase ?? _tripsReviewPhase);
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
    _similarGroups.clear();
    _tripResults.clear();
    _tripSelections.clear();
    _resetTripEdits();
    _commitDestination = '';
    _commitKeepIds = const <String>[];
    _commitTotalBytes = 0;
    _committedDestination = '';
    _commitCopied = 0;
    _commitSkipped = 0;
    _commitErrors = const <String>[];
    if (!keepReturnPhase) {
      _returnPhase = null;
    }
  }
}
