import 'package:grpc/grpc.dart';

import '../generated/kustavi/service.pb.dart';
import '../generated/kustavi/service.pbgrpc.dart' as stub;
import '../state/domain.dart';

/// gRPC metadata header carrying the GUI-generated auth token
/// (spec/frontend.md §3.1).
const String kAuthTokenHeader = 'x-kustavi-auth-token';

Map<String, String> authMetadata(String token) => {
      kAuthTokenHeader: token,
    };

/// The GUI's single door to the back end (spec/frontend.md §4).
///
/// Screens and the wizard controller never touch the generated stubs
/// directly. Returned streams wrap cancellable gRPC calls: cancelling a
/// stream in the GUI cancels the back-end pass (proto spec §2).
abstract interface class KustaviClient {
  Future<GetInfoResponse> getInfo();
  Future<void> shutdown();
  Stream<ScanEvent> scanFolder(ScanFolderRequest request);
  Stream<QualityEvent> runQualityPass({
    required double blurThreshold,
    required double underexposedThreshold,
    required double overexposedThreshold,
  });
  Stream<ModelEvent> ensureModel();
  Stream<JunkEvent> runJunkPass({Iterable<String> skipImageIds});
  Stream<SimilarEvent> runSimilarPass();
  Stream<TripsEvent> runTripsPass(RunTripsPassRequest request);
  Stream<CommitEvent> commit(CommitRequest request);
}

/// Maps a raw gRPC error object to a [BackendError] value (§10.5).
BackendError mapToBackendError(Object error) {
  if (error is BackendError) {
    return error;
  }
  if (error is GrpcError) {
    return mapGrpcError(error);
  }
  return BackendRpc(GrpcError.custom(StatusCode.internal, error.toString()));
}

/// Production [KustaviClient] over gRPC with the auth token embedded in the
/// metadata of every call (spec/frontend.md §3.3, §4).
class GrpcKustaviClient implements KustaviClient {
  GrpcKustaviClient(this._client, {required String token})
      : _options = CallOptions(metadata: authMetadata(token));

  final stub.KustaviClient _client;
  final CallOptions _options;

  @override
  Future<GetInfoResponse> getInfo() {
    return _client
        .getInfo(GetInfoRequest(), options: _options)
        .then(
          (response) => response,
          onError: (Object error) => throw mapToBackendError(error),
        );
  }

  @override
  Future<void> shutdown() {
    return _client
        .shutdown(ShutdownRequest(), options: _options)
        .then(
          (_) {},
          onError: (Object error) => throw mapToBackendError(error),
        );
  }

  @override
  Stream<ScanEvent> scanFolder(ScanFolderRequest request) {
    return _pass(_client.scanFolder(request, options: _options));
  }

  @override
  Stream<QualityEvent> runQualityPass({
    required double blurThreshold,
    required double underexposedThreshold,
    required double overexposedThreshold,
  }) {
    return _pass(
      _client.runQualityPass(
        RunQualityPassRequest()
          ..blurThreshold = blurThreshold
          ..underexposedThreshold = underexposedThreshold
          ..overexposedThreshold = overexposedThreshold,
        options: _options,
      ),
    );
  }

  @override
  Stream<ModelEvent> ensureModel() {
    return _pass(_client.ensureModel(EnsureModelRequest(), options: _options));
  }

  @override
  Stream<JunkEvent> runJunkPass({Iterable<String> skipImageIds = const []}) {
    return _pass(
      _client.runJunkPass(
        RunJunkPassRequest(skipImageIds: skipImageIds),
        options: _options,
      ),
    );
  }

  @override
  Stream<SimilarEvent> runSimilarPass() {
    return _pass(
      _client.runSimilarPass(RunSimilarPassRequest(), options: _options),
    );
  }

  @override
  Stream<TripsEvent> runTripsPass(RunTripsPassRequest request) {
    return _pass(_client.runTripsPass(request, options: _options));
  }

  @override
  Stream<CommitEvent> commit(CommitRequest request) {
    return _pass(_client.commit(request, options: _options));
  }

  Stream<T> _pass<T>(ResponseStream<T> stream) async* {
    try {
      await for (final event in stream) {
        yield event;
      }
    } on Object catch (error) {
      throw mapToBackendError(error);
    }
  }
}
