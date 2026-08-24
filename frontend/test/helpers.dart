import 'dart:async';

import 'package:fixnum/fixnum.dart';
import 'package:grpc/grpc.dart';
import 'package:kustavi/src/backend/process.dart';
import 'package:kustavi/src/generated/kustavi/service.pb.dart' as pb;

/// Shared proto-event builders for tests.

/// Test stand-in for `backendProcessProvider`: never spawns a process and
/// never fails (a failing build schedules riverpod's 200 ms retry timer,
/// which widget tests cannot let dangle).
class InactiveBackendProcess extends BackendProcess {
  @override
  FutureOr<BackendEndpoint> build() => BackendEndpoint(
        handle: ProcessHandle.inactive(token: 'test', port: 0),
        token: 'test',
        port: 0,
      );
}

pb.ScanEvent scanImage(
  String id, {
  String? workingPath,
  int? takenMs,
  (double, double)? gps,
}) {
  final meta = pb.ImageMeta()
    ..id = id
    ..path = '/photos/$id'
    ..name = id
    ..width = 4032
    ..height = 3024
    ..sizeBytes = Int64(8000000)
    ..thumbnailPath = workingPath ?? '/cache/$id';
  if (takenMs != null) {
    meta.takenUnixMs = Int64(takenMs);
  }
  if (gps != null) {
    meta.gps = (pb.GpsPoint()
      ..latitude = gps.$1
      ..longitude = gps.$2);
  }
  return pb.ScanEvent()..image = meta;
}

pb.ScanEvent scanComplete({int images = 0, List<String> errors = const []}) {
  return pb.ScanEvent()
    ..complete = (pb.ScanComplete()
      ..images = images
      ..errors.addAll(errors));
}

pb.QualityEvent qualityFlag(
  String id, {
  double sharpness = 8.5,
  double exposure = 0.9,
  List<pb.QualityReason> reasons = const [pb.QualityReason.BLURRY],
}) {
  return pb.QualityEvent()
    ..flag = (pb.QualityFlag()
      ..imageId = id
      ..reasons.addAll(reasons)
      ..sharpness = sharpness
      ..exposureScore = exposure);
}

pb.ModelEvent modelReady(
        {String model = 'moondream-3.1', int sizeBytes = 1100000000}) =>
    pb.ModelEvent()
      ..ready = (pb.ModelReady()
        ..modelName = model
        ..sizeBytes = Int64(sizeBytes));

pb.ModelEvent modelProgress({
  int doneBytes = 600000000,
  int totalBytes = 1200000000,
  double speedBps = 5000000,
}) {
  return pb.ModelEvent()
    ..progress = (pb.ModelDownloadProgress()
      ..doneBytes = Int64(doneBytes)
      ..totalBytes = Int64(totalBytes)
      ..speedBps = speedBps);
}

pb.JunkEvent junkFlag(
  String id, {
  String reason = 'screenshot',
  double confidence = 0.91,
}) {
  return pb.JunkEvent()
    ..flag = (pb.JunkFlag()
      ..imageId = id
      ..reason = reason
      ..confidence = confidence);
}

pb.SimilarEvent similarGroup(
  int id,
  List<String> ids,
  String keepId,
) {
  return pb.SimilarEvent()
    ..group = (pb.SimilarGroup()
      ..id = id
      ..imageIds.addAll(ids)
      ..recommendedKeepId = keepId
      ..memberScores.addAll(List<double>.filled(ids.length, 0.9)));
}

/// A gRPC "internal" error for step-failure tests.
GrpcError rpcBoom([String message = 'boom']) =>
    GrpcError.custom(StatusCode.internal, message);

pb.TripsEvent tripEvent(
  int id,
  List<String> imageIds, {
  String? folder,
  int? startMs,
  int? endMs,
}) {
  final trip = pb.Trip()
    ..id = id
    ..imageIds.addAll(imageIds);
  if (startMs != null) {
    trip.startUnixMs = Int64(startMs);
  }
  if (endMs != null) {
    trip.endUnixMs = Int64(endMs);
  }
  if (folder != null && folder.isNotEmpty) {
    trip.folder = folder;
  }
  return pb.TripsEvent()..trip = trip;
}
