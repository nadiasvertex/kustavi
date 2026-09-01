/// Byte counts rendered as gigabytes with one decimal, e.g. `2.9 GB`
/// (spec/frontend.md §6.2 model card).
String formatGb(int bytes) => '${(bytes / 1e9).toStringAsFixed(1)} GB';

/// Byte counts with the best-fitting unit, e.g. `1.2 MB`.
String formatBytes(int bytes) {
  const kb = 1024;
  if (bytes < kb) {
    return '$bytes B';
  }
  if (bytes < kb * kb) {
    return '${(bytes / kb).toStringAsFixed(1)} KB';
  }
  if (bytes < kb * kb * kb) {
    return '${(bytes / kb / kb).toStringAsFixed(1)} MB';
  }
  return '${(bytes / kb / kb / kb).toStringAsFixed(1)} GB';
}

/// Integers with thousands separators, e.g. `5,000` (spec/frontend.md §6.2).
String formatInt(int value) {
  final negative = value < 0;
  var digits = (negative ? -value : value).toString();
  final parts = <String>[];
  while (digits.length > 3) {
    parts.insert(0, digits.substring(digits.length - 3));
    digits = digits.substring(0, digits.length - 3);
  }
  parts.insert(0, digits);
  return '${negative ? '-' : ''}${parts.join(',')}';
}

/// Wall-clock time as `HH:mm`, or `yyyy-MM-dd HH:mm` when it falls on a
/// different calendar day than now. No locale dependency.
String formatClockTime(DateTime time) {
  String two(int n) => n.toString().padLeft(2, '0');
  final now = DateTime.now();
  final hm = '${two(time.hour)}:${two(time.minute)}';
  final sameDay =
      time.year == now.year && time.month == now.month && time.day == now.day;
  return sameDay ? hm : '${time.year}-${two(time.month)}-${two(time.day)} $hm';
}

/// Junk-pass expectation copy. While the vision model's per-image cost is
/// still being measured on this machine we say so; once known, we show the
/// measured rate and the projected finish time (spec/frontend.md §6.2 S6).
String junkTimingCaption(double? secondsPerImage, DateTime? estimatedCompletion) {
  if (secondsPerImage == null || estimatedCompletion == null) {
    return 'Profiling how long the vision model takes on your machine…';
  }
  final rate = secondsPerImage >= 10
      ? '${secondsPerImage.round()}'
      : secondsPerImage.toStringAsFixed(1);
  return '$rate seconds per image, estimated completion at '
      '${formatClockTime(estimatedCompletion)}';
}

/// `yyyy-MM-dd HH:mm` without a locale dependency.
String formatDateTime(DateTime dateTime) {
  String two(int n) => n.toString().padLeft(2, '0');
  return '${dateTime.year}-${two(dateTime.month)}-${two(dateTime.day)} '
      '${two(dateTime.hour)}:${two(dateTime.minute)}';
}

/// Duration between two dates, e.g. `3d 14h`.
String formatDuration(DateTime start, DateTime end) {
  final diff = end.difference(start);
  final days = diff.inDays;
  final hours = diff.inHours % 24;
  const d = 'd';
  const h = 'h';
  if (days > 0 && hours > 0) {
    return '$days$d $hours$h';
  }
  if (days > 0) {
    return '$days$d';
  }
  return '${diff.inHours}h';
}
