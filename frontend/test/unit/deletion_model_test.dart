import 'package:flutter_test/flutter_test.dart';
import 'package:kustavi/src/state/decisions.dart';
import 'package:kustavi/src/state/domain.dart';

void main() {
  const noFlags = <String>{};
  const noKeepers = <String, String>{};

  group('isMarkedForDeletion (§8 hierarchy)', () {
    test('explicit deletion beats every rule', () {
      // Not flagged, yet still marked: explicit deletion is rule 1.
      const plan = DeletionIntent(explicitDeleted: {'a.jpg'});
      expect(
        isMarkedForDeletion(
          plan,
          'a.jpg',
          step: DeletionStep.quality,
          qualityFlagged: noFlags,
          junkFlagged: noFlags,
          similarKeepers: noKeepers,
        ),
        isTrue,
      );
    });

    test('explicit keep beats the step default', () {
      const plan = DeletionIntent(explicitKept: {'a.jpg'});
      expect(
        isMarkedForDeletion(
          plan,
          'a.jpg',
          step: DeletionStep.quality,
          qualityFlagged: const {'a.jpg'},
          junkFlagged: noFlags,
          similarKeepers: noKeepers,
        ),
        isFalse,
      );
    });

    test('quality default marks flagged images only', () {
      const plan = DeletionIntent();
      expect(
        isMarkedForDeletion(
          plan,
          'a.jpg',
          step: DeletionStep.quality,
          qualityFlagged: const {'a.jpg'},
          junkFlagged: noFlags,
          similarKeepers: noKeepers,
        ),
        isTrue,
      );
      expect(
        isMarkedForDeletion(
          plan,
          'b.jpg',
          step: DeletionStep.quality,
          qualityFlagged: const {'a.jpg'},
          junkFlagged: noFlags,
          similarKeepers: noKeepers,
        ),
        isFalse,
      );
    });

    test('similar default marks non-keepers only', () {
      const plan = DeletionIntent();
      const keepers = <String, String>{'a.jpg': 'b.jpg', 'b.jpg': 'b.jpg'};
      expect(
        isMarkedForDeletion(
          plan,
          'a.jpg',
          step: DeletionStep.similar,
          qualityFlagged: noFlags,
          junkFlagged: noFlags,
          similarKeepers: keepers,
        ),
        isTrue,
      );
      expect(
        isMarkedForDeletion(
          plan,
          'b.jpg',
          step: DeletionStep.similar,
          qualityFlagged: noFlags,
          junkFlagged: noFlags,
          similarKeepers: keepers,
        ),
        isFalse,
      );
    });

    test('without a step, only explicit rules apply', () {
      const plan = DeletionIntent();
      expect(
        isMarkedForDeletion(
          plan,
          'a.jpg',
          step: null,
          qualityFlagged: const {'a.jpg'},
          junkFlagged: noFlags,
          similarKeepers: noKeepers,
        ),
        isFalse,
      );
    });
  });

  group('DeletionIntent', () {
    test('toggle without intent marks for deletion', () {
      const plan = DeletionIntent();
      final marked = plan.withToggle('a.jpg');
      expect(marked.explicitDeleted, contains('a.jpg'));
      expect(marked.explicitKept, isEmpty);
    });

    test('toggling an explicit deletion keeps it', () {
      const plan = DeletionIntent(explicitDeleted: {'a.jpg'});
      final kept = plan.withToggle('a.jpg');
      expect(kept.explicitKept, contains('a.jpg'));
      expect(kept.explicitDeleted, isEmpty);
    });

    test('keepAll clears marks on the given ids', () {
      const plan = DeletionIntent(explicitDeleted: {'a.jpg', 'b.jpg'});
      final kept = plan.withKeepAll(['a.jpg']);
      expect(kept.explicitDeleted, contains('b.jpg'));
      expect(kept.explicitDeleted, isNot(contains('a.jpg')));
    });

    test('markAll removes keeps on the given ids', () {
      const plan = DeletionIntent(explicitKept: {'a.jpg', 'b.jpg'});
      final marked = plan.withMarkAll(['b.jpg']);
      expect(marked.explicitKept, contains('a.jpg'));
      expect(marked.explicitKept, isNot(contains('b.jpg')));
    });

    test('collections are immutable', () {
      final plan = const DeletionIntent().withMarked('a.jpg');
      expect(
        () => (plan.explicitDeleted as Set).add('b.jpg'),
        throwsUnsupportedError,
      );
    });

    test('keeper reassignment is isolated per group', () {
      final plan = const DeletionIntent().withKeeper(1, 'b.jpg');
      expect(plan.userKeeperOf(1), 'b.jpg');
      expect(plan.userKeeperOf(2), isNull);
    });
  });

  group('similarKeeperMap', () {
    const groups = [
      SimilarGroupInfo(
        id: 1,
        memberIds: ['a.jpg', 'b.jpg'],
        recommendedKeepId: 'a.jpg',
        memberScores: [0.93, 0.81],
      ),
    ];

    test('falls back to the back-end recommendation', () {
      expect(
        similarKeeperMap(const DeletionIntent(), groups),
        {
          'a.jpg': 'a.jpg',
          'b.jpg': 'a.jpg',
        },
      );
    });

    test('user reassignment overrides the recommendation', () {
      final plan = const DeletionIntent().withKeeper(1, 'b.jpg');
      expect(
        similarKeeperMap(plan, groups),
        {
          'a.jpg': 'b.jpg',
          'b.jpg': 'b.jpg',
        },
      );
    });
  });
}
