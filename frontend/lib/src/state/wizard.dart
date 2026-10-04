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
import 'trip_edits.dart';

part 'wizard.g.dart';
part 'wizard_batches.dart';
part 'wizard_commit.dart';
part 'wizard_deletion.dart';
part 'wizard_passes.dart';
part 'wizard_session.dart';
part 'wizard_trips.dart';

/// The wizard controller (spec/frontend.md §6, §9).
///
/// Owns the incremental image index (the GUI's single source of truth for
/// image metadata, §5) and the linear phase machine. One pass stream is in
/// flight at a time; `EnsureModel` is exempt (it runs in [ModelStatus]).
///
/// This file holds the state fields, `build`, the read-only accessors and the
/// pass plumbing. The behaviour lives in extensions in the part files, one per
/// responsibility: `wizard_session.dart` (scan, resume, persistence),
/// `wizard_trips.dart` (organize), `wizard_batches.dart` (batch menu),
/// `wizard_passes.dart` (quality, junk, duplicates, video),
/// `wizard_deletion.dart` (marks and the final review) and
/// `wizard_commit.dart` (copy).
@Riverpod(keepAlive: true)
class Wizard extends _$Wizard {
  final Map<String, ImageInfo> _images = {};
  final List<String> _orderedIds = [];
  final Map<String, QualityFlagInfo> _qualityFlags = {};
  final Map<String, JunkFlagInfo> _junkFlags = {};
  final List<SimilarGroupInfo> _similarGroups = [];
  final Map<String, VideoFlagInfo> _videoFlags = {};
  int _videoTotal = 0;

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

  /// Saved trip edits waiting for the trips pass to finish on a resume; null
  /// when no resume is in progress.
  String? _pendingTripEdits;

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
  int _commitAlreadyPresent = 0;
  List<String> _commitErrors = const <String>[];

  /// Latest `EstimateCommit` answer and the destination it was computed for.
  pb.EstimateCommitResponse? _commitEstimate;
  String _commitEstimateFor = '';
  Timer? _estimateTimer;

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

  Map<String, VideoFlagInfo> get videoFlags => UnmodifiableMapView(_videoFlags);

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
    _persistTripEdits();
    _tripResults.clear();
    state = const AsyncValue.data(WizardTripsRunning());
    final client = ref.read(kustaviClientProvider).requireValue;
    _subscribe(
      client.runTripsPass(_tripsRequest()),
      _onTripsEvent,
      _onTripsDone,
    );
  }

  /// Flagged count the stored metrics give at the current slider values.
  int? _previewFlagged;
  int _previewSeq = 0;

  @override
  FutureOr<WizardPhase> build() async {
    ref.onDispose(() => _estimateTimer?.cancel());

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

  // Riverpod keeps `state` and `ref` protected, so the extensions in the part
  // files reach them through these.
  AsyncValue<WizardPhase> get _state => state;
  set _state(AsyncValue<WizardPhase> value) => state = value;
  Ref get _ref => ref;

  static bool _isRestingPhase(WizardPhase phase) =>
      phase is WizardConfirmFolder ||
      phase is WizardQualityReview ||
      phase is WizardSimilarReview ||
      phase is WizardJunkReview ||
      phase is WizardVideoReview ||
      phase is WizardTripsReview ||
      phase is WizardBatchMenu ||
      phase is WizardDeletionReview ||
      phase is WizardCommitSummary;

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

  final List<String> _runAllQueue = [];
  WizardStep? _runAllStep;

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
    _pendingTripEdits = null;
    _commitDestination = '';
    _commitKeepIds = const <String>[];
    _commitTotalBytes = 0;
    _committedDestination = '';
    _commitCopied = 0;
    _commitSkipped = 0;
    _commitAlreadyPresent = 0;
    _commitErrors = const <String>[];
    _estimateTimer?.cancel();
    _commitEstimate = null;
    _commitEstimateFor = '';
    _resumeTargetStep = null;
    _resuming = false;
    if (!keepReturnPhase) {
      _returnPhase = null;
    }
  }
}
