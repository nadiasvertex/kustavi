import 'package:flutter_test/flutter_test.dart';
import 'package:kustavi/src/ui/format.dart';

void main() {
  group('formatClockTime', () {
    test('same day → HH:mm', () {
      final now = DateTime.now();
      final target = DateTime(now.year, now.month, now.day, 9, 5);
      expect(formatClockTime(target), '09:05');
    });

    test('different day → yyyy-MM-dd HH:mm', () {
      final now = DateTime.now();
      final tomorrow = now.add(const Duration(days: 2));
      final target =
          DateTime(tomorrow.year, tomorrow.month, tomorrow.day, 14, 30);
      String two(int n) => n.toString().padLeft(2, '0');
      expect(
        formatClockTime(target),
        '${target.year}-${two(target.month)}-${two(target.day)} 14:30',
      );
    });
  });

  group('junkTimingCaption', () {
    test('still profiling when the rate is unknown', () {
      expect(
        junkTimingCaption(null, null),
        'Profiling how long the vision model takes on your machine…',
      );
    });

    test('sub-10s rate keeps one decimal', () {
      final now = DateTime.now();
      final eta = DateTime(now.year, now.month, now.day, 10, 15);
      expect(
        junkTimingCaption(2.4, eta),
        '2.4 seconds per image, estimated completion at 10:15',
      );
    });

    test('rate of 10s or more is rounded to a whole number', () {
      final now = DateTime.now();
      final eta = DateTime(now.year, now.month, now.day, 23, 0);
      expect(
        junkTimingCaption(12.7, eta),
        '13 seconds per image, estimated completion at 23:00',
      );
    });
  });
}
