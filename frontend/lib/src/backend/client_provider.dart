import 'dart:async';

import 'package:riverpod_annotation/riverpod_annotation.dart';

import '../generated/kustavi/service.pb.dart';
import '../generated/kustavi/service.pbgrpc.dart' as stub;
import '../state/domain.dart';
import 'channel.dart';
import 'client.dart';
import 'process.dart';

part 'client_provider.g.dart';

/// The [KustaviClient] used across the app (spec/frontend.md §4, §9).
///
/// In tests this is overridden with a [FakeKustaviClient].
@Riverpod(keepAlive: true)
FutureOr<KustaviClient> kustaviClient(Ref ref) async {
  final endpoint = await ref.watch(backendProcessProvider.future);
  final channel = await ref.watch(grpcChannelProvider.future);
  return GrpcKustaviClient(stub.KustaviClient(channel), token: endpoint.token);
}

/// In-memory [KustaviClient] for tests (spec/frontend.md §4).
///
/// Each pass method replays its scripted event list as a stream, optionally
/// injecting an error before closing. [ensureModel] can be kept open (a
/// download in progress) until [closeModelStream] is called.
class FakeKustaviClient implements KustaviClient {
  FakeKustaviClient({
    GetInfoResponse? info,
    this.scanEvents = const <ScanEvent>[],
    this.qualityEvents = const <QualityEvent>[],
    this.modelEvents = const <ModelEvent>[],
    this.junkEvents = const <JunkEvent>[],
    this.similarEvents = const <SimilarEvent>[],
    this.tripsEvents = const <TripsEvent>[],
    this.videoEvents = const <VideoEvent>[],
    this.commitEvents = const <CommitEvent>[],
    EstimateCommitResponse? estimateCommitResponse,
    this.inspectSessionResponse,
    this.sessionResults,
    this.scanError,
    this.qualityError,
    this.junkError,
    this.similarError,
    this.videoError,
    this.ensureModelError,
    this.modelStreamStaysOpen = false,
    this.scanStreamStaysOpen = false,
  }) : info = info ?? _defaultInfo(),
       estimateCommitResponse =
           estimateCommitResponse ?? EstimateCommitResponse(fits: true);

  static GetInfoResponse _defaultInfo() {
    return GetInfoResponse()
      ..version = 'fake'
      ..modelName = 'qwen2.5-vl-3b';
  }

  /// Back end version/model metadata returned by `GetInfo`.
  final GetInfoResponse info;

  final List<ScanEvent> scanEvents;
  final List<QualityEvent> qualityEvents;
  final List<ModelEvent> modelEvents;
  final List<JunkEvent> junkEvents;
  final List<SimilarEvent> similarEvents;
  final List<TripsEvent> tripsEvents;
  final List<VideoEvent> videoEvents;
  final List<CommitEvent> commitEvents;

  /// Injected into the stream of the corresponding pass.
  final Object? scanError;
  final Object? qualityError;
  final Object? junkError;
  final Object? similarError;
  final Object? videoError;
  final Object? ensureModelError;

  /// Keep the [ensureModel] stream open until [closeModelStream].
  final bool modelStreamStaysOpen;

  /// Keep the [scanFolder] stream open until [closeScanStream] so tests can
  /// observe the in-progress scan screen.
  final bool scanStreamStaysOpen;

  /// Returned by [inspectSession]; defaults to "no saved session".
  final InspectSessionResponse? inspectSessionResponse;

  /// Returned by [getSessionResults] on resume; defaults to empty.
  final GetSessionResultsResponse? sessionResults;

  ScanFolderRequest? lastScanRequest;
  RunQualityPassRequest? lastQualityRequest;
  CommitRequest? lastCommitRequest;

  /// Returned by `EstimateCommit`; defaults to "fits".
  final EstimateCommitResponse estimateCommitResponse;
  CommitRequest? lastEstimateRequest;
  RunTripsPassRequest? lastTripsRequest;
  List<String> lastJunkSkipIds = const [];
  List<String> lastSimilarSkipIds = const [];
  List<String> lastVideoSkipIds = const [];

  /// Scope and batch key of the most recent run of each scoped pass, so tests
  /// can assert what a batch run asked the back end for.
  List<String> lastQualityScope = const [];
  List<String> lastJunkScope = const [];
  List<String> lastSimilarScope = const [];
  List<String> lastVideoScope = const [];
  List<String> lastPreviewScope = const [];
  String lastQualityBatchKey = '';
  String lastJunkBatchKey = '';
  String lastSimilarBatchKey = '';
  String lastVideoBatchKey = '';
  final List<SaveSessionStateRequest> savedSessionStates = [];
  int shutdownCount = 0;
  int qualityPassCount = 0;

  final Completer<void> _modelDone = Completer<void>();
  StreamController<ScanEvent>? _scanController;

  /// Closes a [ensureModel] stream that was left open.
  void closeModelStream() {
    if (!_modelDone.isCompleted) {
      _modelDone.complete();
    }
  }

  /// Pushes an event into the open scan stream.
  void pushScanEvent(ScanEvent event) {
    final controller = _scanController;
    if (controller != null && !controller.isClosed) {
      controller.add(event);
    }
  }

  /// Closes the open scan stream.
  void closeScanStream() {
    final controller = _scanController;
    if (controller != null && !controller.isClosed) {
      unawaited(controller.close());
    }
    _scanController = null;
  }

  @override
  Future<GetInfoResponse> getInfo() async => info;

  @override
  Future<void> shutdown() async {
    shutdownCount++;
  }

  @override
  Future<InspectSessionResponse> inspectSession(String folder) async {
    return inspectSessionResponse ?? InspectSessionResponse();
  }

  @override
  Future<GetSessionResultsResponse> getSessionResults() async {
    return sessionResults ?? GetSessionResultsResponse();
  }

  @override
  Future<void> saveSessionState(SaveSessionStateRequest request) async {
    savedSessionStates.add(request);
  }

  /// Returned by [previewQualityThresholds]; defaults to an empty count.
  PreviewQualityThresholdsResponse? previewResponse;

  @override
  Future<PreviewQualityThresholdsResponse> previewQualityThresholds({
    required double blurThreshold,
    required double underexposedThreshold,
    required double overexposedThreshold,
    Iterable<String> scopeImageIds = const [],
  }) async {
    lastPreviewScope = scopeImageIds.toList(growable: false);
    return previewResponse ?? PreviewQualityThresholdsResponse();
  }

  @override
  Stream<ScanEvent> scanFolder(ScanFolderRequest request) {
    lastScanRequest = request;
    if (!scanStreamStaysOpen) {
      return _script(scanEvents, scanError);
    }
    // Closed later via closeScanStream (deliberately left open).
    // ignore: close_sinks
    final controller = StreamController<ScanEvent>();
    _scanController = controller;
    for (final event in scanEvents) {
      controller.add(event);
    }
    return controller.stream;
  }

  @override
  Stream<QualityEvent> runQualityPass({
    required double blurThreshold,
    required double underexposedThreshold,
    required double overexposedThreshold,
    Iterable<String> scopeImageIds = const [],
    String batchKey = '',
  }) {
    qualityPassCount++;
    lastQualityScope = scopeImageIds.toList(growable: false);
    lastQualityBatchKey = batchKey;
    lastQualityRequest = RunQualityPassRequest()
      ..blurThreshold = blurThreshold
      ..underexposedThreshold = underexposedThreshold
      ..overexposedThreshold = overexposedThreshold;
    return _script(qualityEvents, qualityError);
  }

  @override
  Stream<ModelEvent> ensureModel() {
    final controller = StreamController<ModelEvent>();
    for (final event in modelEvents) {
      controller.add(event);
    }
    if (ensureModelError != null && !modelStreamStaysOpen) {
      controller.addError(mapToBackendError(ensureModelError!));
      unawaited(controller.close());
    } else if (!modelStreamStaysOpen) {
      unawaited(controller.close());
    } else {
      _modelDone.future.then((_) => controller.close());
    }
    return controller.stream;
  }

  @override
  Stream<JunkEvent> runJunkPass({
    Iterable<String> skipImageIds = const [],
    Iterable<String> scopeImageIds = const [],
    String batchKey = '',
  }) {
    lastJunkSkipIds = skipImageIds.toList(growable: false);
    lastJunkScope = scopeImageIds.toList(growable: false);
    lastJunkBatchKey = batchKey;
    return _script(junkEvents, junkError);
  }

  @override
  Stream<SimilarEvent> runSimilarPass({
    Iterable<String> skipImageIds = const [],
    Iterable<String> scopeImageIds = const [],
    String batchKey = '',
  }) {
    lastSimilarSkipIds = skipImageIds.toList(growable: false);
    lastSimilarScope = scopeImageIds.toList(growable: false);
    lastSimilarBatchKey = batchKey;
    return _script(similarEvents, similarError);
  }

  @override
  Stream<TripsEvent> runTripsPass(RunTripsPassRequest request) {
    lastTripsRequest = request;
    return Stream.fromIterable(tripsEvents);
  }

  @override
  Stream<VideoEvent> runVideoPass({
    Iterable<String> skipVideoIds = const [],
    Iterable<String> scopeImageIds = const [],
    String batchKey = '',
  }) {
    lastVideoSkipIds = skipVideoIds.toList(growable: false);
    lastVideoScope = scopeImageIds.toList(growable: false);
    lastVideoBatchKey = batchKey;
    return _script(videoEvents, videoError);
  }

  @override
  Stream<CommitEvent> commit(CommitRequest request) {
    lastCommitRequest = request;
    return Stream.fromIterable(commitEvents);
  }

  @override
  Future<EstimateCommitResponse> estimateCommit(CommitRequest request) async {
    lastEstimateRequest = request;
    return estimateCommitResponse;
  }

  Stream<T> _script<T>(List<T> events, Object? error) {
    if (error == null) {
      return Stream.fromIterable(events);
    }
    final controller = StreamController<T>();
    for (final event in events) {
      controller.add(event);
    }
    controller.addError(
      error is BackendError ? error : mapToBackendError(error),
    );
    unawaited(controller.close());
    return controller.stream;
  }
}
