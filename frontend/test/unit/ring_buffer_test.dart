import 'package:flutter_test/flutter_test.dart';
import 'package:kustavi/src/backend/process.dart';

void main() {
  group('RingBuffer (§3.1)', () {
    test('accumulates lines until full', () {
      final buffer = RingBuffer(maxBytes: 1024);
      buffer.addLine('hello');
      buffer.addLine('world');
      expect(buffer.text, 'hello\nworld\n');
      expect(buffer.sizeBytes, 'hello\nworld\n'.length);
    });

    test('trims oldest bytes to stay within capacity', () {
      final buffer = RingBuffer(maxBytes: 64);
      for (var i = 0; i < 50; i++) {
        buffer.addLine('line-$i');
      }
      expect(buffer.sizeBytes, lessThanOrEqualTo(64));
      // The most recent line survives the trim.
      expect(buffer.text, endsWith('line-49\n'));
      expect(buffer.text, isNot(contains('line-0')));
    });

    test('a single oversized line is truncated', () {
      final buffer = RingBuffer(maxBytes: 16);
      buffer.addLine('x' * 100);
      expect(buffer.sizeBytes, lessThanOrEqualTo(16));
      expect(buffer.text, 'x' * 16);
    });
  });
}
