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
    this.commitEvents = const <CommitEvent>[],
    this.scanError,
    this.qualityError,
    this.junkError,
    this.similarError,
    this.ensureModelError,
    this.modelStreamStaysOpen = false,
  }) : _info = info;

  static GetInfoResponse _defaultInfo() {
    return GetInfoResponse()
      ..version = 'fake'
      ..modelName = 'moondream-3.1';
  }

  final GetInfoResponse? _info;

  /// Back end version/model metadata returned by `GetInfo`.
  late final GetInfoResponse info = _info ?? _defaultInfo();

  final List<ScanEvent> scanEvents;
  final List<QualityEvent> qualityEvents;
  final List<ModelEvent> modelEvents;
  final List<JunkEvent> junkEvents;
  final List<SimilarEvent> similarEvents;
  final List<TripsEvent> tripsEvents;
  final List<CommitEvent> commitEvents;

  /// Injected into the stream of the corresponding pass.
  final Object? scanError;
  final Object? qualityError;
  final Object? junkError;
  final Object? similarError;
  final Object? ensureModelError;

  /// Keep the [ensureModel] stream open until [closeModelStream].
  final bool modelStreamStaysOpen;

  ScanFolderRequest? lastScanRequest;
  CommitRequest? lastCommitRequest;
  RunTripsPassRequest? lastTripsRequest;
  int shutdownCount = 0;

  final Completer<void> _modelDone = Completer<void>();

  /// Closes a [ensureModel] stream that was left open.
  void closeModelStream() {
    if (!_modelDone.isCompleted) {
      _modelDone.complete();
    }
  }

  @override
  Future<GetInfoResponse> getInfo() async => info;

  @override
  Future<void> shutdown() async {
    shutdownCount++;
  }

  @override
  Stream<ScanEvent> scanFolder(ScanFolderRequest request) {
    lastScanRequest = request;
    return _script(scanEvents, scanError);
  }

  @override
  Stream<QualityEvent> runQualityPass() =>
      _script(qualityEvents, qualityError);

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
  Stream<JunkEvent> runJunkPass() => _script(junkEvents, junkError);

  @override
  Stream<SimilarEvent> runSimilarPass() =>
      _script(similarEvents, similarError);

  @override
  Stream<TripsEvent> runTripsPass(RunTripsPassRequest request) {
    lastTripsRequest = request;
    return Stream.fromIterable(tripsEvents);
  }

  @override
  Stream<CommitEvent> commit(CommitRequest request) {
    lastCommitRequest = request;
    return Stream.fromIterable(commitEvents);
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
