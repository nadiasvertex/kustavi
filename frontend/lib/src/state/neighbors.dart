import 'domain.dart';

/// How close in time two photos must be to count as neighbors.
const Duration neighborWindow = Duration(minutes: 2);

/// Finds the photos taken next to each flagged photo, so a reviewer can see
/// whether a better shot of the same moment exists.
///
/// Photos are ordered by capture time (photos without one are left out), and
/// each flagged photo gets up to [perSide] photos before and after it that are
/// within [neighborWindow], are not videos, and are not in [excluded].
Map<String, List<ImageInfo>> findNeighbors({
  required Iterable<ImageInfo> candidates,
  required Set<String> flaggedIds,
  Set<String> excluded = const <String>{},
  int perSide = 2,
}) {
  final timed =
      candidates
          .where((image) => image.taken != null && !image.isVideo)
          .toList()
        ..sort((a, b) => a.taken!.compareTo(b.taken!));
  final result = <String, List<ImageInfo>>{};
  for (var i = 0; i < timed.length; i++) {
    final image = timed[i];
    if (!flaggedIds.contains(image.id)) {
      continue;
    }
    final found = <ImageInfo>[];
    var taken = 0;
    for (var j = i - 1; j >= 0 && taken < perSide; j--) {
      if (image.taken!.difference(timed[j].taken!) > neighborWindow) {
        break;
      }
      if (!excluded.contains(timed[j].id)) {
        found.insert(0, timed[j]);
        taken++;
      }
    }
    taken = 0;
    for (var j = i + 1; j < timed.length && taken < perSide; j++) {
      if (timed[j].taken!.difference(image.taken!) > neighborWindow) {
        break;
      }
      if (!excluded.contains(timed[j].id)) {
        found.add(timed[j]);
        taken++;
      }
    }
    if (found.isNotEmpty) {
      result[image.id] = found;
    }
  }
  return result;
}
