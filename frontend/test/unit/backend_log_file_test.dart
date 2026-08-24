import 'dart:io';
import 'dart:math';

import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:kustavi/src/backend/process.dart';

void main() {
  group('defaultBackendLogFile', () {
    late Directory tempDir;

    setUp(() async {
      tempDir = await Directory.systemTemp.createTemp('kustavi_log_test');
    });

    tearDown(() async {
      await tempDir.delete();
    });

    final fixedTime = DateTime(2026, 8, 24, 14, 30, 12);
    final namePattern =
        RegExp(r'^kustavi-backend-\d{8}-\d{6}-[0-9a-z]{8}\.log$');

    test('lives in the given temp directory with a timestamped name', () {
      final path = defaultBackendLogFile(
        tempDir: tempDir,
        now: fixedTime,
        random: Random(42),
      );
      expect(p.dirname(path), tempDir.path);
      expect(p.basename(path), matches(namePattern));
    });

    test('is deterministic for a fixed time and random', () {
      final first = defaultBackendLogFile(
        tempDir: tempDir,
        now: fixedTime,
        random: Random(42),
      );
      final second = defaultBackendLogFile(
        tempDir: tempDir,
        now: fixedTime,
        random: Random(42),
      );
      expect(first, second);
    });

    test('varies with the random suffix', () {
      final first = defaultBackendLogFile(
        tempDir: tempDir,
        now: fixedTime,
        random: Random(1),
      );
      final second = defaultBackendLogFile(
        tempDir: tempDir,
        now: fixedTime,
        random: Random(2),
      );
      expect(first, isNot(second));
    });
  });
}
