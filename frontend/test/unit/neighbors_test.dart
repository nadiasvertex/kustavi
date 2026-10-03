import 'package:flutter_test/flutter_test.dart';
import 'package:kustavi/src/state/domain.dart';
import 'package:kustavi/src/state/neighbors.dart';

ImageInfo _img(String id, int seconds, {bool video = false}) => ImageInfo(
  id: id,
  path: '/p/$id',
  name: id,
  sizeBytes: 1,
  width: 1,
  height: 1,
  taken: DateTime(2026, 1, 1).add(Duration(seconds: seconds)),
  workingImagePath: '/c/$id',
  isVideo: video,
);

void main() {
  group('findNeighbors', () {
    test('returns unflagged photos on both sides, in time order', () {
      final images = [
        _img('d', 30),
        _img('a', 0),
        _img('c', 20),
        _img('b', 10),
      ];
      final result = findNeighbors(
        candidates: images,
        flaggedIds: {'b'},
        excluded: {'b'},
      );
      expect(result['b']!.map((i) => i.id), ['a', 'c', 'd']);
    });

    test('skips excluded photos, videos, untimed photos and distant shots', () {
      final images = [
        _img('a', 0),
        _img('flag', 10),
        _img('also-flag', 20),
        _img('vid', 30, video: true),
        _img('far', 10000),
      ];
      final result = findNeighbors(
        candidates: images,
        flaggedIds: {'flag', 'also-flag'},
        excluded: {'flag', 'also-flag'},
      );
      expect(result['flag']!.map((i) => i.id), ['a']);
      expect(result['also-flag']!.map((i) => i.id), ['a']);
    });

    test('omits flagged photos with no neighbors', () {
      final result = findNeighbors(
        candidates: [_img('only', 0)],
        flaggedIds: {'only'},
        excluded: {'only'},
      );
      expect(result, isEmpty);
    });
  });
}
