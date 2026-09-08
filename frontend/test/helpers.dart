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

pb.ScanEvent scanComplete({
  int images = 0,
  List<String> errors = const [],
  bool resumed = false,
  int resumeStep = 0,
}) {
  return pb.ScanEvent()
    ..complete = (pb.ScanComplete()
      ..images = images
      ..errors.addAll(errors)
      ..resumed = resumed
      ..resumeStep = resumeStep);
}

pb.InspectSessionResponse inspectSession({
  bool hasSession = true,
  int imageCount = 0,
  int resumeStep = 0,
}) {
  return pb.InspectSessionResponse()
    ..hasSession = hasSession
    ..imageCount = imageCount
    ..resumeStep = resumeStep;
}

pb.GetSessionResultsResponse sessionResults({
  int resumeStep = 0,
  int videoTotal = 0,
  bool qualityDone = false,
  bool similarDone = false,
  bool junkDone = false,
  bool videoDone = false,
  List<pb.QualityFlag> qualityFlags = const [],
  List<pb.SimilarGroup> similarGroups = const [],
  List<pb.JunkFlag> junkFlags = const [],
  List<pb.VideoFlag> videoFlags = const [],
  Map<String, bool> decisions = const {}, // image id -> true means delete
  Map<int, String> groupKeepers = const {},
}) {
  final response = pb.GetSessionResultsResponse()
    ..resumeStep = resumeStep
    ..videoTotal = videoTotal
    ..qualityDone = qualityDone
    ..similarDone = similarDone
    ..junkDone = junkDone
    ..videoDone = videoDone
    ..qualityFlags.addAll(qualityFlags)
    ..similarGroups.addAll(similarGroups)
    ..junkFlags.addAll(junkFlags)
    ..videoFlags.addAll(videoFlags);
  decisions.forEach((id, remove) {
    response.decisions.add(pb.DecisionEntry()
      ..imageId = id
      ..decision = remove ? pb.Decision.DELETE : pb.Decision.KEEP);
  });
  groupKeepers.forEach((k, v) => response.groupKeepers[k] = v);
  return response;
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

pb.ModelEvent modelReady({
  String model = 'qwen2.5-vl-3b',
  int sizeBytes = 1100000000,
}) => pb.ModelEvent()
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

pb.VideoEvent videoFlag(
  String id, {
  String reason = 'too_short',
  double confidence = 1.0,
}) {
  return pb.VideoEvent()
    ..flag = (pb.VideoFlag()
      ..videoId = id
      ..reason = reason
      ..confidence = confidence);
}

pb.SimilarEvent similarGroup(int id, List<String> ids, String keepId) {
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

pb.CommitEvent commitProgress({
  int done = 0,
  int total = 0,
  String currentName = '',
}) {
  return pb.CommitEvent()
    ..progress = (pb.CommitProgress()
      ..done = done
      ..total = total
      ..currentName = currentName);
}

pb.CommitEvent commitComplete({
  int copied = 0,
  int skipped = 0,
  List<String> errors = const [],
}) {
  return pb.CommitEvent()
    ..complete = (pb.CommitComplete()
      ..copied = copied
      ..skipped = skipped
      ..errors.addAll(errors));
}

pb.TripsEvent tripEvent(
  int id,
  List<String> imageIds, {
  String? folder,
  String? folderSlug,
  String? placeName,
  int? startMs,
  int? endMs,
}) {
  final trip = pb.Trip()
    ..id = id
    ..imageIds.addAll(imageIds);
  if (placeName != null && placeName.isNotEmpty) {
    trip.placeName = placeName;
  }
  if (startMs != null) {
    trip.startUnixMs = Int64(startMs);
  }
  if (endMs != null) {
    trip.endUnixMs = Int64(endMs);
  }
  if (folder != null && folder.isNotEmpty) {
    trip.folder = folder;
  }
  if (folderSlug != null && folderSlug.isNotEmpty) {
    trip.folderSlug = folderSlug;
  }
  return pb.TripsEvent()..trip = trip;
}
