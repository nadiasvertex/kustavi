import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:kustavi/src/backend/client_provider.dart';
import 'package:kustavi/src/state/decisions.dart';
import 'package:kustavi/src/state/domain.dart';
import 'package:kustavi/src/state/model_status.dart';
import 'package:kustavi/src/state/phases.dart';
import 'package:kustavi/src/state/wizard.dart';

import '../helpers.dart';

/// Flushed microtasks until [done] holds (pass streams deliver async).
Future<void> pumpUntil(
  ProviderContainer container,
  bool Function() done, {
  int maxIterations = 200,
}) async {
  for (var i = 0; i < maxIterations && !done(); i++) {
    await Future<void>.delayed(Duration.zero);
  }
}

ProviderContainer makeContainer(FakeKustaviClient client) {
  return ProviderContainer(
    overrides: [
      kustaviClientProvider.overrideWith((ref) => client),
    ],
  );
}

Future<void> reachConfirmFolder(
  ProviderContainer container,
  FakeKustaviClient client, {
  String folder = '/photos',
}) async {
  await pumpUntil(
    container,
    () => container.read(wizardProvider).value is WizardStart,
  );
  container.read(wizardProvider.notifier).selectFolder(folder);
  await pumpUntil(
    container,
    () => container.read(wizardProvider).value is WizardConfirmFolder,
  );
  expect(client.lastScanRequest?.folder, folder);
}

void main() {
  late ProviderContainer container;

  tearDown(() => container.dispose());

  group('folder initialization workflow (§6, §12)', () {
    test('S0 select → S1 scan → S2 confirm with incremental index', () async {
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.jpg'),
          scanImage('b.jpg'),
          scanComplete(images: 2),
        ],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);

      final phase = container.read(wizardProvider).value
          as WizardConfirmFolder;
      expect(phase.imageCount, 2);
      expect(phase.folder, '/photos');
      expect(client.lastScanRequest?.recursive, isTrue);

      final wizard = container.read(wizardProvider.notifier);
      expect(wizard.imageIds, ['a.jpg', 'b.jpg']);
      expect(wizard.images['a.jpg']!.workingImagePath, '/cache/a.jpg');
    });

    test('zero-image scan → no-images phase', () async {
      final client = FakeKustaviClient(
        scanEvents: [scanComplete(images: 0)],
      );
      container = makeContainer(client);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardStart,
      );
      container.read(wizardProvider.notifier).selectFolder('/empty');

      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardNoImages,
      );
      expect(
        (container.read(wizardProvider).value as WizardNoImages).folder,
        '/empty',
      );
    });

    test('scan errors are surfaced on the confirm phase', () async {
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.jpg'),
          scanComplete(images: 1, errors: ['broken.jpg: truncated']),
        ],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);

      expect(
        (container.read(wizardProvider).value as WizardConfirmFolder)
            .scanErrors,
        ['broken.jpg: truncated'],
      );
    });

    test('continue → quality pass → S4 review with flags', () async {
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.jpg'),
          scanImage('b.jpg'),
          scanComplete(images: 2),
        ],
        qualityEvents: [qualityFlag('a.jpg')],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);

      container.read(wizardProvider.notifier).continueFromConfirm();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardQualityReview,
      );
      final phase = container.read(wizardProvider).value
          as WizardQualityReview;
      expect(phase.flaggedCount, 1);
      expect(phase.totalImages, 2);
      expect(
        container.read(wizardProvider.notifier).qualityFlags.keys,
        contains('a.jpg'),
      );
    });

    test('keep-all / mark-all update the deletion plan', () async {
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.jpg'),
          scanComplete(images: 1),
        ],
        qualityEvents: [qualityFlag('a.jpg')],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      container.read(wizardProvider.notifier).continueFromConfirm();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardQualityReview,
      );

      final wizard = container.read(wizardProvider.notifier);
      wizard.keepAllQualityFlagged();
      var plan = container.read(deletionPlanProvider);
      expect(plan.explicitKept, contains('a.jpg'));

      wizard.markAllQualityFlagged();
      plan = container.read(deletionPlanProvider);
      expect(plan.explicitDeleted, contains('a.jpg'));
      expect(plan.explicitKept, isNot(contains('a.jpg')));
    });

    test('quality RPC error → wizard error state (§10.2)', () async {
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.jpg'),
          scanComplete(images: 1),
        ],
        qualityError: rpcBoom('quality exploded'),
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);

      container.read(wizardProvider.notifier).continueFromConfirm();
      await pumpUntil(
        container,
        () =>
            container.read(wizardProvider) is AsyncError<Object?> &&
            container.read(wizardProvider).value == null,
      );
      final error = (container.read(wizardProvider)
          as AsyncError<Object?>)
          .error;
      expect(error, isA<BackendRpc>());
      expect((error as BackendRpc).message, 'quality exploded');

      // [Back] on the error screen returns to the pre-pass phase (S2).
      container.read(wizardProvider.notifier).goBackFromError();
      expect(
        container.read(wizardProvider).value,
        isA<WizardConfirmFolder>(),
      );
    });

    test('threshold change republishes the review phase (notifies, reruns)',
        () async {
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.jpg'),
          scanImage('b.jpg'),
          scanComplete(images: 2),
        ],
        qualityEvents: [qualityFlag('a.jpg')],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      container.read(wizardProvider.notifier).continueFromConfirm();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardQualityReview,
      );

      final wizard = container.read(wizardProvider.notifier);
      final before = container.read(wizardProvider).value
          as WizardQualityReview;
      expect(before.rerunEnabled, isFalse);

      var notifications = 0;
      container.listen(wizardProvider, (_, _) => notifications++);

      wizard.setBlurThreshold(250);
      expect(notifications, 1);
      final after = container.read(wizardProvider).value
          as WizardQualityReview;
      expect(identical(before, after), isFalse);
      expect(after.rerunEnabled, isTrue);
      // The republished phase keeps the review's counts.
      expect(after.flaggedCount, before.flaggedCount);
      expect(after.totalImages, before.totalImages);

      // A write with an unchanged value publishes nothing.
      wizard.setBlurThreshold(250);
      expect(notifications, 1);
      expect(identical(container.read(wizardProvider).value, after), isTrue);
    });

    test('rerunQualityPass keeps the image index and applies new thresholds',
        () async {
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.jpg'),
          scanImage('b.jpg'),
          scanComplete(images: 2),
        ],
        qualityEvents: [qualityFlag('a.jpg')],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      final wizard = container.read(wizardProvider.notifier);
      wizard.continueFromConfirm();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardQualityReview,
      );

      wizard.setBlurThreshold(250);
      wizard.setUnderexposedThreshold(0.5);
      wizard.rerunQualityPass();

      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardQualityReview,
      );
      final phase = container.read(wizardProvider).value
          as WizardQualityReview;
      expect(client.qualityPassCount, 2);
      // The index survived the rerun: the header total and the S2 grid
      // (via [backFromQuality]) stay valid.
      expect(phase.flaggedCount, 1);
      expect(phase.totalImages, 2);
      expect(wizard.images, hasLength(2));
      // The pass ran with the adjusted thresholds.
      expect(client.lastQualityRequest?.blurThreshold, 250);
      expect(client.lastQualityRequest?.underexposedThreshold, 0.5);
    });

    test('rerunQualityPass without threshold changes is a no-op', () async {
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.jpg'),
          scanComplete(images: 1),
        ],
        qualityEvents: [qualityFlag('a.jpg')],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      container.read(wizardProvider.notifier).continueFromConfirm();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardQualityReview,
      );

      final before = container.read(wizardProvider).value;
      container.read(wizardProvider.notifier).rerunQualityPass();
      expect(identical(container.read(wizardProvider).value, before), isTrue);
      expect(client.qualityPassCount, 1);
    });

    test('continue with ready model → similar pass → S8 review', () async {
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.jpg'),
          scanImage('b.jpg'),
          scanComplete(images: 2),
        ],
        similarEvents: [similarGroup(1, ['a.jpg', 'b.jpg'], 'a.jpg')],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      container.read(wizardProvider.notifier).continueFromConfirm();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardQualityReview,
      );
      container.read(wizardProvider.notifier).continueFromQuality();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardSimilarReview,
      );
      final phase = container.read(wizardProvider).value
          as WizardSimilarReview;
      expect(phase.groupCount, 1);
    });

    test('continue from similar → trips → junk pass → junk review', () async {
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.jpg'),
          scanImage('b.jpg'),
          scanComplete(images: 2),
        ],
        modelEvents: [modelReady()],
        similarEvents: [similarGroup(1, ['a.jpg', 'b.jpg'], 'a.jpg')],
        junkEvents: [junkFlag('b.jpg', reason: 'meme')],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      container.read(wizardProvider.notifier).continueFromConfirm();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardQualityReview,
      );
      container.read(wizardProvider.notifier).continueFromQuality();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardSimilarReview,
      );

      await pumpUntil(
        container,
        () =>
            container.read(modelStatusProvider).value is ModelPrepReady,
      );

      container.read(wizardProvider.notifier).continueFromSimilar();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardTripsReview,
      );

      container.read(wizardProvider.notifier).continueFromTrips();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardJunkReview,
      );
      expect(
        container.read(wizardProvider.notifier).junkFlags.keys,
        contains('b.jpg'),
      );
    });

    test('continue from junk → trips → commit summary', () async {
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.jpg'),
          scanImage('b.jpg'),
          scanComplete(images: 2),
        ],
        modelEvents: [modelReady()],
        similarEvents: [similarGroup(1, ['a.jpg', 'b.jpg'], 'a.jpg')],
        junkEvents: [junkFlag('b.jpg', reason: 'meme')],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      container.read(wizardProvider.notifier).continueFromConfirm();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardQualityReview,
      );
      container.read(wizardProvider.notifier).continueFromQuality();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardSimilarReview,
      );

      await pumpUntil(
        container,
        () =>
            container.read(modelStatusProvider).value is ModelPrepReady,
      );

      container.read(wizardProvider.notifier).continueFromSimilar();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardTripsReview,
      );

      container.read(wizardProvider.notifier).continueFromTrips();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardJunkReview,
      );
      expect(
        container.read(wizardProvider.notifier).junkFlags.keys,
        contains('b.jpg'),
      );

      container.read(wizardProvider.notifier).continueFromJunk();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardTripsReview,
      );
    });

    test('back from confirm discards results (fresh session → S0)', () async {
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.jpg'),
          scanComplete(images: 1),
        ],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);

      container.read(wizardProvider.notifier).backFromConfirm();
      expect(
        container.read(wizardProvider).value,
        isA<WizardStart>(),
      );
      expect(
        container.read(wizardProvider.notifier).imageIds,
        isEmpty,
      );
    });

    test('resetToStart clears the deletion plan (S13 start over)', () async {
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.jpg'),
          scanComplete(images: 1),
        ],
      );
      container = makeContainer(client);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardStart,
      );
      container
          .read(deletionPlanProvider.notifier)
          .mark('a.jpg');
      expect(
        container.read(deletionPlanProvider).explicitDeleted,
        contains('a.jpg'),
      );

      container.read(wizardProvider.notifier).resetToStart();
      expect(
        container.read(deletionPlanProvider).explicitDeleted,
        isEmpty,
      );
    });
  });
}
