import 'package:fixnum/fixnum.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:grpc/grpc.dart';
import 'package:kustavi/src/generated/kustavi/service.pb.dart';
import 'package:kustavi/src/generated/kustavi/service.pbgrpc.dart';

class FakeKustavi extends KustaviServiceBase {
  @override
  Future<GetInfoResponse> getInfo(
          ServiceCall call, GetInfoRequest request) =>
      Future.value(GetInfoResponse()..version = 'test');

  @override
  Future<ShutdownResponse> shutdown(
          ServiceCall call, ShutdownRequest request) =>
      Future.value(ShutdownResponse());

  @override
  Stream<ScanEvent> scanFolder(
          ServiceCall call, ScanFolderRequest request) =>
      Stream.empty();

  @override
  Stream<QualityEvent> runQualityPass(
          ServiceCall call, RunQualityPassRequest request) =>
      Stream.empty();

  @override
  Stream<ModelEvent> ensureModel(
          ServiceCall call, EnsureModelRequest request) =>
      Stream.empty();

  @override
  Stream<JunkEvent> runJunkPass(ServiceCall call, RunJunkPassRequest request) =>
      Stream.empty();

  @override
  Stream<SimilarEvent> runSimilarPass(
          ServiceCall call, RunSimilarPassRequest request) =>
      Stream.empty();

  @override
  Stream<TripsEvent> runTripsPass(
          ServiceCall call, RunTripsPassRequest request) =>
      Stream.empty();

  @override
  Stream<CommitEvent> commit(ServiceCall call, CommitRequest request) =>
      Stream.empty();
}

void main() {
  group('generated service proto', () {
    test('service is named kustavi.Kustavi', () {
      expect(FakeKustavi().$name, 'kustavi.Kustavi');
    });

    test('client exposes unary GetInfo and streaming passes', () {
      final channel = ClientChannel('127.0.0.1:1');
      addTearDown(() => channel.terminate());
      final client = KustaviClient(channel);
      expect(
        client.getInfo,
        isA<ResponseFuture<GetInfoResponse> Function(GetInfoRequest)>(),
      );
      expect(
        client.scanFolder,
        isA<ResponseStream<ScanEvent> Function(ScanFolderRequest)>(),
      );
      expect(
        client.commit,
        isA<ResponseStream<CommitEvent> Function(CommitRequest)>(),
      );
    });

    test('ScanEvent progress round-trips through the wire format', () {
      final event = ScanEvent()
        ..progress = (ScanProgress()
          ..filesSeen = 250
          ..imagesFound = 100
          ..currentPath = '2024/paris');
      final decoded = ScanEvent.fromBuffer(event.writeToBuffer());
      expect(decoded.hasProgress(), isTrue);
      expect(decoded.progress.filesSeen, 250);
      expect(decoded.progress.imagesFound, 100);
      expect(decoded.progress.currentPath, '2024/paris');
    });

    test('ImageMeta optional fields honor presence', () {
      final withMeta = ImageMeta()
        ..id = '2024/paris/IMG_0001.jpg'
        ..path = '/photos/2024/paris/IMG_0001.jpg'
        ..name = 'IMG_0001.jpg'
        ..width = 4032
        ..height = 3024
        ..sizeBytes = Int64(1234567)
        ..takenUnixMs = Int64(1700000000000)
        ..gps = (GpsPoint()
          ..latitude = 48.85
          ..longitude = 2.35)
        ..thumbnailPath = '/cache/IMG_0001.jpg';
      final decoded = ImageMeta.fromBuffer(withMeta.writeToBuffer());
      expect(decoded.id, '2024/paris/IMG_0001.jpg');
      expect(decoded.hasTakenUnixMs(), isTrue);
      expect(decoded.takenUnixMs, 1700000000000);
      expect(decoded.hasGps(), isTrue);
      expect(decoded.gps.latitude, closeTo(48.85, 1e-9));

      final withoutMeta = ImageMeta()..id = 'a.jpg';
      final decodedBare = ImageMeta.fromBuffer(withoutMeta.writeToBuffer());
      expect(decodedBare.hasTakenUnixMs(), isFalse);
      expect(decodedBare.hasGps(), isFalse);
    });

    test('QualityFlag enum reasons round-trip', () {
      final flag = QualityFlag()
        ..imageId = 'x.jpg'
        ..reasons.addAll([QualityReason.BLURRY, QualityReason.OVER_EXPOSED])
        ..sharpness = 12.5
        ..exposureScore = 0.9;
      final decoded = QualityFlag.fromBuffer(flag.writeToBuffer());
      expect(decoded.imageId, 'x.jpg');
      expect(
        decoded.reasons,
        [QualityReason.BLURRY, QualityReason.OVER_EXPOSED],
      );
      expect(decoded.sharpness, closeTo(12.5, 1e-9));
      expect(decoded.exposureScore, closeTo(0.9, 1e-9));
    });

    test('SimilarGroup members stay parallel to scores', () {
      final group = SimilarGroup()
        ..id = 7
        ..imageIds.addAll(['b.jpg', 'a.jpg'])
        ..recommendedKeepId = 'a.jpg'
        ..memberScores.addAll([0.81, 0.93]);
      final decoded = SimilarGroup.fromBuffer(group.writeToBuffer());
      expect(decoded.imageIds, ['b.jpg', 'a.jpg']);
      expect(decoded.recommendedKeepId, 'a.jpg');
      expect(decoded.memberScores, [closeTo(0.81, 1e-9), closeTo(0.93, 1e-9)]);
    });

    test('Trip centroid is optional', () {
      final noGps = Trip()
        ..id = 1
        ..startUnixMs = Int64(1000)
        ..endUnixMs = Int64(2000)
        ..imageIds.addAll(['a.jpg', 'b.jpg']);
      final decodedNoGps = Trip.fromBuffer(noGps.writeToBuffer());
      expect(decodedNoGps.hasCentroid(), isFalse);
      expect(decodedNoGps.startUnixMs, 1000);
      expect(decodedNoGps.imageIds, ['a.jpg', 'b.jpg']);

      final withGps = noGps.clone()
        ..centroid = (GpsPoint()
          ..latitude = 48.85
          ..longitude = 2.35);
      final decodedWithGps = Trip.fromBuffer(withGps.writeToBuffer());
      expect(decodedWithGps.hasCentroid(), isTrue);
      expect(decodedWithGps.centroid.latitude, closeTo(48.85, 1e-9));
    });

    test('RunTripsPassRequest carries thresholds', () {
      final request = RunTripsPassRequest()
        ..maxGapHours = 24
        ..maxDistanceKm = 100;
      final decoded = RunTripsPassRequest.fromBuffer(request.writeToBuffer());
      expect(decoded.maxGapHours, 24);
      expect(decoded.maxDistanceKm, 100);
    });
  });
}
