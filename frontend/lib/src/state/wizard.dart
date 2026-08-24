import 'dart:async';
import 'dart:collection';

import 'package:grpc/grpc.dart';
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
  final List<TripInfo> _tripResults = [];
  final Map<int, Set<String>> _tripSelections = {};

  // Quality pass thresholds (user-adjustable, defaults match back end)
  static const double _kDefaultBlurThreshold = 100.0;
  static const double _kDefaultUnderexposedThreshold = 0.3;
  static const double _kDefaultOverexposedThreshold = 0.3;

  double _blurThreshold = _kDefaultBlurThreshold;
  double _underexposedThreshold = _kDefaultUnderexposedThreshold;
  double _overexposedThreshold = _kDefaultOverexposedThreshold;

  // Public accessors for the UI (quality review screen)
  double get blurThreshold => _blurThreshold;
  double get underexposedThreshold => _underexposedThreshold;
  double get overexposedThreshold => _overexposedThreshold;

  StreamSubscription<dynamic>? _passSubscription;
  bool _cancelRequested = false;
  pb.ScanComplete? _pendingScanComplete;
  WizardPhase? _returnPhase;

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

  List<TripInfo> get tripResults =>
      List<TripInfo>.unmodifiable(_tripResults);

  Map<int, Set<String>> get tripSelections =>
      UnmodifiableMapView(_tripSelections);

  WizardQualityReview get _qualityReviewPhase => WizardQualityReview(
        flaggedCount: _qualityFlags.length,
        totalImages: _images.length,
        rerunEnabled: _hasThresholdChanges,
      );

  bool get _hasThresholdChanges =>
      _blurThreshold != _kDefaultBlurThreshold ||
      _underexposedThreshold != _kDefaultUnderexposedThreshold ||
      _overexposedThreshold != _kDefaultOverexposedThreshold;

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
    state = AsyncValue.data(_qualityReviewPhase);
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
    if (state.value is! WizardQualityReview) {
      return;
    }
    _clearPassResults(keepReturnPhase: true);
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
    _blurThreshold = value;
    state = AsyncValue.data(state.value!);
  }

  void setUnderexposedThreshold(double value) {
    _underexposedThreshold = value;
    state = AsyncValue.data(state.value!);
  }

  void setOverexposedThreshold(double value) {
    _overexposedThreshold = value;
    state = AsyncValue.data(state.value!);
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
    _junkFlags.clear();
    _returnPhase = state.value;
    _similarGroups.clear();
    state = const AsyncValue.data(WizardSimilarRunning());
    final client = ref.read(kustaviClientProvider).requireValue;
    _subscribe(
      client.runSimilarPass(),
      _onSimilarEvent,
      _onSimilarDone,
    );
  }

  void continueFromJunk() {
    if (state.value is! WizardJunkReview) {
      return;
    }
    _returnPhase = state.value;
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

  void continueFromTrips() {
    if (state.value is! WizardTripsReview) {
      return;
    }
    _returnPhase = _tripsReviewPhase;
    if (_modelReady) {
      _startJunkPass();
    } else {
      state = const AsyncValue.data(WizardJunkPrep());
    }
  }

  void cancelTrips() {
    if (state.value is! WizardTripsRunning) {
      return;
    }
    _cancelPass();
    state = AsyncValue.data(_returnPhase ?? _similarReviewPhase);
  }

  pb.RunTripsPassRequest _tripsRequest() {
    return pb.RunTripsPassRequest()
      ..maxGapHours = 48
      ..maxDistanceKm = 300;
  }

  void cancelJunkPrep() {
    if (state.value is! WizardJunkPrep) {
      return;
    }
    ref.read(modelStatusProvider.notifier).cancelDownload();
    state = AsyncValue.data(_returnPhase ?? _qualityReviewPhase);
  }

  void _startJunkPass() {
    state = const AsyncValue.data(WizardJunkRunning());
    final client = ref.read(kustaviClientProvider).requireValue;
    _subscribe(client.runJunkPass(), _onJunkEvent, _onJunkDone);
  }

  WizardSimilarReview get _similarReviewPhase => WizardSimilarReview(
        groupCount: _similarGroups.length,
        markedCount: _similarMarkedCount(),
      );

  WizardTripsReview get _tripsReviewPhase => WizardTripsReview(
        tripCount: _tripResults.length,
        markedCount: _tripsMarkedCount(),
        trips: _tripResults,
      );

  int _tripsMarkedCount() {
    int count = 0;
    for (final trip in _tripResults) {
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
        state = AsyncValue.data(
          WizardJunkRunning(
            done: event.progress.done,
            total: event.progress.total,
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
    state = AsyncValue.data(_returnPhase ?? _qualityReviewPhase);
  }

  // --- S7 ----------------------------------------------------------------

  void backFromJunk() {
    if (state.value is! WizardJunkReview) {
      return;
    }
    state = AsyncValue.data(_returnPhase ?? _qualityReviewPhase);
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

  void continueFromSimilar() {
    if (state.value is! WizardSimilarReview) {
      return;
    }
    _returnPhase = state.value;
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

  void backFromTrips() {
    if (state.value is! WizardTripsReview) {
      return;
    }
    state = AsyncValue.data(_returnPhase ?? _similarReviewPhase);
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

  void _clearPassResults({bool keepReturnPhase = false}) {
    _cancelPass();
    _pendingScanComplete = null;
    _images.clear();
    _orderedIds.clear();
    _qualityFlags.clear();
    _junkFlags.clear();
    _similarGroups.clear();
    _tripResults.clear();
    _tripSelections.clear();
    if (!keepReturnPhase) {
      _returnPhase = null;
    }
  }
}
