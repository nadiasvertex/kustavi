import 'package:flutter_test/flutter_test.dart';
import 'package:grpc/grpc.dart';
import 'package:kustavi/src/generated/kustavi/service.pb.dart' as pb;
import 'package:kustavi/src/state/domain.dart';

import '../helpers.dart';

void main() {
  group('ImageInfo.fromMeta', () {
    test('maps the working preview path and optional fields', () {
      final info = ImageInfo.fromMeta(
        scanImage('a.jpg', workingPath: '/cache/a.jpg', takenMs: 1700000000000, gps: (48.85, 2.35)).image,
      );
      expect(info.id, 'a.jpg');
      expect(info.workingImagePath, '/cache/a.jpg');
      expect(info.taken, isNotNull);
      expect(info.gps, (48.85, 2.35));
    });

    test('absent optional fields map to null', () {
      final info = ImageInfo.fromMeta(scanImage('a.jpg').image);
      expect(info.taken, isNull);
      expect(info.gps, isNull);
    });
  });

  group('QualityFlagInfo.fromFlag', () {
    test('maps reasons and scores', () {
      final flag = QualityFlagInfo.fromFlag(
        qualityFlag(
          'a.jpg',
          sharpness: 7.5,
          exposure: 0.88,
          reasons: [
            pb.QualityReason.BLURRY,
            pb.QualityReason.OVER_EXPOSED,
          ],
        ).flag,
      );
      expect(flag.imageId, 'a.jpg');
      expect(
        flag.reasons,
        [QualityReasonTag.blurry, QualityReasonTag.overExposed],
      );
      expect(flag.sharpness, 7.5);
      expect(flag.exposureScore, 0.88);
    });
  });

  group('mapGrpcError (§10.5)', () {
    test('NOT_FOUND maps to BackendNotFound', () {
      final error = mapGrpcError(
        const GrpcError.custom(StatusCode.notFound, 'missing'),
      );
      expect(error, isA<BackendNotFound>());
    });

    test('UNAVAILABLE maps to a crash', () {
      final error = mapGrpcError(
        const GrpcError.custom(StatusCode.unavailable, 'gone'),
      );
      expect(error, isA<BackendCrashed>());
    });

    test('other statuses surface verbatim as BackendRpc', () {
      final error = mapGrpcError(rpcBoom('boom'));
      expect(error, isA<BackendRpc>());
      expect(error.message, 'boom');
    });
  });
}
