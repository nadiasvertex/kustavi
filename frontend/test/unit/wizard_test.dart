import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:kustavi/src/backend/client_provider.dart';
import 'package:kustavi/src/generated/kustavi/service.pb.dart' as pb;
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
    overrides: [kustaviClientProvider.overrideWith((ref) => client)],
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

/// Confirm → Organize (trips pass) → batch menu. With no scripted trips every
/// photo lands in the single "Unassigned" batch.
Future<void> reachBatchMenu(
  ProviderContainer container,
  FakeKustaviClient client,
) async {
  final wizard = container.read(wizardProvider.notifier);
  wizard.continueFromConfirm();
  await pumpUntil(
    container,
    () => container.read(wizardProvider).value is WizardTripsReview,
  );
  wizard.continueFromTrips();
  expect(container.read(wizardProvider).value, isA<WizardBatchMenu>());
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

      final phase = container.read(wizardProvider).value as WizardConfirmFolder;
      expect(phase.imageCount, 2);
      expect(phase.folder, '/photos');
      expect(client.lastScanRequest?.recursive, isTrue);

      final wizard = container.read(wizardProvider.notifier);
      expect(wizard.imageIds, ['a.jpg', 'b.jpg']);
      expect(wizard.images['a.jpg']!.workingImagePath, '/cache/a.jpg');
    });

    test('zero-image scan → no-images phase', () async {
      final client = FakeKustaviClient(scanEvents: [scanComplete(images: 0)]);
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

      await reachBatchMenu(container, client);
      container
          .read(wizardProvider.notifier)
          .startBatchPass(WizardStep.quality);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardQualityReview,
      );
      final phase = container.read(wizardProvider).value as WizardQualityReview;
      expect(phase.flaggedCount, 1);
      expect(phase.totalImages, 2);
      expect(
        container.read(wizardProvider.notifier).qualityFlags.keys,
        contains('a.jpg'),
      );
    });

    test('keep-all / mark-all update the deletion plan', () async {
      final client = FakeKustaviClient(
        scanEvents: [scanImage('a.jpg'), scanComplete(images: 1)],
        qualityEvents: [qualityFlag('a.jpg')],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      await reachBatchMenu(container, client);
      container
          .read(wizardProvider.notifier)
          .startBatchPass(WizardStep.quality);
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
        scanEvents: [scanImage('a.jpg'), scanComplete(images: 1)],
        qualityError: rpcBoom('quality exploded'),
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);

      await reachBatchMenu(container, client);
      container
          .read(wizardProvider.notifier)
          .startBatchPass(WizardStep.quality);
      await pumpUntil(
        container,
        () =>
            container.read(wizardProvider) is AsyncError<Object?> &&
            container.read(wizardProvider).value == null,
      );
      final error =
          (container.read(wizardProvider) as AsyncError<Object?>).error;
      expect(error, isA<BackendRpc>());
      expect((error as BackendRpc).message, 'quality exploded');

      // [Back] on the error screen returns to the batch menu with the
      // session intact.
      container.read(wizardProvider.notifier).goBackFromError();
      expect(container.read(wizardProvider).value, isA<WizardBatchMenu>());
      expect(container.read(wizardProvider.notifier).imageIds, ['a.jpg']);
    });

    test(
      'threshold change republishes the review phase (notifies, reruns)',
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
        await reachBatchMenu(container, client);
        container
            .read(wizardProvider.notifier)
            .startBatchPass(WizardStep.quality);
        await pumpUntil(
          container,
          () => container.read(wizardProvider).value is WizardQualityReview,
        );

        final wizard = container.read(wizardProvider.notifier);
        final before =
            container.read(wizardProvider).value as WizardQualityReview;
        expect(before.rerunEnabled, isFalse);

        var notifications = 0;
        container.listen(wizardProvider, (_, _) => notifications++);

        wizard.setBlurThreshold(250);
        expect(notifications, 1);
        final after =
            container.read(wizardProvider).value as WizardQualityReview;
        expect(identical(before, after), isFalse);
        expect(after.rerunEnabled, isTrue);
        // The republished phase keeps the review's counts.
        expect(after.flaggedCount, before.flaggedCount);
        expect(after.totalImages, before.totalImages);

        // A write with an unchanged value publishes nothing.
        wizard.setBlurThreshold(250);
        expect(notifications, 1);
        expect(identical(container.read(wizardProvider).value, after), isTrue);
      },
    );

    test(
      'rerunQualityPass keeps the image index and applies new thresholds',
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
        await reachBatchMenu(container, client);
        wizard.startBatchPass(WizardStep.quality);
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
        final phase =
            container.read(wizardProvider).value as WizardQualityReview;
        expect(client.qualityPassCount, 2);
        // The index survived the rerun: the header total and the S2 grid
        // (via [backFromQuality]) stay valid.
        expect(phase.flaggedCount, 1);
        expect(phase.totalImages, 2);
        expect(wizard.images, hasLength(2));
        // The pass ran with the adjusted thresholds.
        expect(client.lastQualityRequest?.blurThreshold, 250);
        expect(client.lastQualityRequest?.underexposedThreshold, 0.5);
      },
    );

    test('rerunQualityPass without threshold changes is a no-op', () async {
      final client = FakeKustaviClient(
        scanEvents: [scanImage('a.jpg'), scanComplete(images: 1)],
        qualityEvents: [qualityFlag('a.jpg')],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      await reachBatchMenu(container, client);
      container
          .read(wizardProvider.notifier)
          .startBatchPass(WizardStep.quality);
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
        similarEvents: [
          similarGroup(1, ['a.jpg', 'b.jpg'], 'a.jpg'),
        ],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      await reachBatchMenu(container, client);
      container
          .read(wizardProvider.notifier)
          .startBatchPass(WizardStep.quality);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardQualityReview,
      );
      container.read(wizardProvider.notifier)
        ..closeBatchReview()
        ..startBatchPass(WizardStep.duplicates);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardSimilarReview,
      );
      final phase = container.read(wizardProvider).value as WizardSimilarReview;
      expect(phase.groupCount, 1);
    });

    test('continue from similar → junk pass → junk review', () async {
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.jpg'),
          scanImage('b.jpg'),
          scanComplete(images: 2),
        ],
        modelEvents: [modelReady()],
        similarEvents: [
          similarGroup(1, ['a.jpg', 'b.jpg'], 'a.jpg'),
        ],
        junkEvents: [junkFlag('b.jpg', reason: 'meme')],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      await reachBatchMenu(container, client);
      container
          .read(wizardProvider.notifier)
          .startBatchPass(WizardStep.quality);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardQualityReview,
      );
      container.read(wizardProvider.notifier)
        ..closeBatchReview()
        ..startBatchPass(WizardStep.duplicates);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardSimilarReview,
      );

      await pumpUntil(
        container,
        () => container.read(modelStatusProvider).value is ModelPrepReady,
      );

      container.read(wizardProvider.notifier)
        ..closeBatchReview()
        ..startBatchPass(WizardStep.junk);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardJunkReview,
      );
      expect(
        container.read(wizardProvider.notifier).junkFlags.keys,
        contains('b.jpg'),
      );
    });

    test('junk pass skips images already marked for deletion', () async {
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.jpg'),
          scanImage('b.jpg'),
          scanComplete(images: 2),
        ],
        modelEvents: [modelReady()],
        qualityEvents: [qualityFlag('a.jpg')],
        similarEvents: const [],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      await reachBatchMenu(container, client);
      container
          .read(wizardProvider.notifier)
          .startBatchPass(WizardStep.quality);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardQualityReview,
      );
      // The quality-flagged image stays marked for deletion by default.
      container.read(wizardProvider.notifier)
        ..closeBatchReview()
        ..startBatchPass(WizardStep.duplicates);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardSimilarReview,
      );
      await pumpUntil(
        container,
        () => container.read(modelStatusProvider).value is ModelPrepReady,
      );
      container.read(wizardProvider.notifier)
        ..closeBatchReview()
        ..startBatchPass(WizardStep.junk);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardJunkReview,
      );
      expect(client.lastJunkSkipIds, contains('a.jpg'));
      expect(client.lastJunkSkipIds, isNot(contains('b.jpg')));
    });

    test('continue from junk → video → trips → commit summary', () async {
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.jpg'),
          scanImage('b.jpg'),
          scanComplete(images: 2),
        ],
        modelEvents: [modelReady()],
        similarEvents: [
          similarGroup(1, ['a.jpg', 'b.jpg'], 'a.jpg'),
        ],
        junkEvents: [junkFlag('b.jpg', reason: 'meme')],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      await reachBatchMenu(container, client);
      container
          .read(wizardProvider.notifier)
          .startBatchPass(WizardStep.quality);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardQualityReview,
      );
      container.read(wizardProvider.notifier)
        ..closeBatchReview()
        ..startBatchPass(WizardStep.duplicates);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardSimilarReview,
      );

      await pumpUntil(
        container,
        () => container.read(modelStatusProvider).value is ModelPrepReady,
      );

      container.read(wizardProvider.notifier)
        ..closeBatchReview()
        ..startBatchPass(WizardStep.junk);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardJunkReview,
      );
      expect(
        container.read(wizardProvider.notifier).junkFlags.keys,
        contains('b.jpg'),
      );

      container.read(wizardProvider.notifier)
        ..closeBatchReview()
        ..startBatchPass(WizardStep.video);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardVideoReview,
      );

      container.read(wizardProvider.notifier)
        ..closeBatchReview()
        ..continueFromBatches();
      expect(container.read(wizardProvider).value, isA<WizardCommitSummary>());
    });

    test('commit summary → Copy → committing → done', () async {
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.jpg'),
          scanImage('b.jpg'),
          scanComplete(images: 2),
        ],
        modelEvents: [modelReady()],
        similarEvents: const [],
        junkEvents: [junkFlag('b.jpg', reason: 'meme')],
        commitEvents: [
          commitProgress(done: 1, total: 1, currentName: 'a.jpg'),
          commitComplete(copied: 1, skipped: 0),
        ],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      final wizard = container.read(wizardProvider.notifier);
      await reachBatchMenu(container, client);
      wizard.startBatchPass(WizardStep.quality);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardQualityReview,
      );
      wizard
        ..closeBatchReview()
        ..startBatchPass(WizardStep.duplicates);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardSimilarReview,
      );
      await pumpUntil(
        container,
        () => container.read(modelStatusProvider).value is ModelPrepReady,
      );
      wizard
        ..closeBatchReview()
        ..startBatchPass(WizardStep.junk);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardJunkReview,
      );
      wizard
        ..closeBatchReview()
        ..startBatchPass(WizardStep.video);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardVideoReview,
      );
      wizard
        ..closeBatchReview()
        ..continueFromBatches();
      expect(container.read(wizardProvider).value, isA<WizardCommitSummary>());

      // The suggested destination is a `<source-name>-kept` sibling.
      final summary =
          container.read(wizardProvider).value as WizardCommitSummary;
      expect(summary.destination, '/photos-kept');
      expect(summary.keepCount, 1); // b.jpg is junk-flagged → left behind
      expect(summary.leftBehindCount, 1);

      // Editing the field republishes a fresh phase.
      wizard.setCommitDestination('/exports/keep');
      expect(
        (container.read(wizardProvider).value as WizardCommitSummary)
            .destination,
        '/exports/keep',
      );

      wizard.startCommit();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardDone,
      );
      expect(client.lastCommitRequest?.destination, '/exports/keep');
      expect(client.lastCommitRequest?.keepIds, ['a.jpg']);

      final done = container.read(wizardProvider).value as WizardDone;
      expect(done.copiedCount, 1);
      expect(done.destination, '/exports/keep');
    });

    test('cancel committing returns to the commit summary', () async {
      final client = FakeKustaviClient(
        scanEvents: [scanImage('a.jpg'), scanComplete(images: 1)],
        modelEvents: [modelReady()],
        similarEvents: const [],
        commitEvents: [commitProgress(done: 0, total: 1)],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      final wizard = container.read(wizardProvider.notifier);
      await reachBatchMenu(container, client);
      wizard.startBatchPass(WizardStep.quality);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardQualityReview,
      );
      wizard
        ..closeBatchReview()
        ..startBatchPass(WizardStep.duplicates);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardSimilarReview,
      );
      await pumpUntil(
        container,
        () => container.read(modelStatusProvider).value is ModelPrepReady,
      );
      wizard
        ..closeBatchReview()
        ..startBatchPass(WizardStep.junk);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardJunkReview,
      );
      wizard
        ..closeBatchReview()
        ..startBatchPass(WizardStep.video);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardVideoReview,
      );
      wizard
        ..closeBatchReview()
        ..continueFromBatches();
      expect(container.read(wizardProvider).value, isA<WizardCommitSummary>());

      wizard.startCommit();
      wizard.cancelCommit();
      expect(container.read(wizardProvider).value, isA<WizardCommitSummary>());
    });

    test('continue from junk → video pass → video review', () async {
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.mp4'),
          scanImage('b.mp4'),
          scanComplete(images: 2),
        ],
        modelEvents: [modelReady()],
        similarEvents: const [],
        videoEvents: [videoFlag('b.mp4', reason: 'static')],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      final wizard = container.read(wizardProvider.notifier);
      await reachBatchMenu(container, client);
      wizard.startBatchPass(WizardStep.quality);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardQualityReview,
      );
      wizard
        ..closeBatchReview()
        ..startBatchPass(WizardStep.duplicates);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardSimilarReview,
      );
      await pumpUntil(
        container,
        () => container.read(modelStatusProvider).value is ModelPrepReady,
      );
      wizard
        ..closeBatchReview()
        ..startBatchPass(WizardStep.junk);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardJunkReview,
      );
      wizard
        ..closeBatchReview()
        ..startBatchPass(WizardStep.video);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardVideoReview,
      );
      expect(wizard.videoFlags.keys, contains('b.mp4'));
      final review = container.read(wizardProvider).value as WizardVideoReview;
      expect(review.flaggedCount, 1);
    });

    test('video pass skips images already marked for deletion', () async {
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.mp4'),
          scanImage('b.mp4'),
          scanComplete(images: 2),
        ],
        modelEvents: [modelReady()],
        qualityEvents: [qualityFlag('a.mp4')],
        similarEvents: const [],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      final wizard = container.read(wizardProvider.notifier);
      await reachBatchMenu(container, client);
      wizard.startBatchPass(WizardStep.quality);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardQualityReview,
      );
      // The quality-flagged video stays marked for deletion by default.
      wizard
        ..closeBatchReview()
        ..startBatchPass(WizardStep.duplicates);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardSimilarReview,
      );
      await pumpUntil(
        container,
        () => container.read(modelStatusProvider).value is ModelPrepReady,
      );
      wizard
        ..closeBatchReview()
        ..startBatchPass(WizardStep.junk);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardJunkReview,
      );
      wizard
        ..closeBatchReview()
        ..startBatchPass(WizardStep.video);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardVideoReview,
      );
      expect(client.lastVideoSkipIds, contains('a.mp4'));
      expect(client.lastVideoSkipIds, isNot(contains('b.mp4')));
    });

    test('video review: keep all / mark all bulk actions', () async {
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.mp4'),
          scanImage('b.mp4'),
          scanComplete(images: 2),
        ],
        modelEvents: [modelReady()],
        similarEvents: const [],
        videoEvents: [
          videoFlag('a.mp4', reason: 'too_short'),
          videoFlag('b.mp4', reason: 'corrupt'),
        ],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      final wizard = container.read(wizardProvider.notifier);
      await reachBatchMenu(container, client);
      wizard.startBatchPass(WizardStep.quality);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardQualityReview,
      );
      wizard
        ..closeBatchReview()
        ..startBatchPass(WizardStep.duplicates);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardSimilarReview,
      );
      await pumpUntil(
        container,
        () => container.read(modelStatusProvider).value is ModelPrepReady,
      );
      wizard
        ..closeBatchReview()
        ..startBatchPass(WizardStep.junk);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardJunkReview,
      );
      wizard
        ..closeBatchReview()
        ..startBatchPass(WizardStep.video);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardVideoReview,
      );

      wizard.keepAllVideoFlagged();
      final plan = container.read(deletionPlanProvider);
      expect(plan.explicitKept, containsAll(<String>['a.mp4', 'b.mp4']));

      wizard.markAllVideoFlagged();
      final plan2 = container.read(deletionPlanProvider);
      expect(plan2.explicitDeleted, containsAll(<String>['a.mp4', 'b.mp4']));
    });

    test('trips review: move photos between trips, create, unassign, rerun', () async {
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.jpg'),
          scanImage('b.jpg'),
          scanImage('c.jpg'),
          scanComplete(images: 3),
        ],
        modelEvents: [modelReady()],
        similarEvents: const [],
        tripsEvents: [
          tripEvent(
            0,
            ['a.jpg', 'b.jpg'],
            folder: 'Rome, Italy · April 2026',
            placeName: 'Rome, Italy',
            startMs: 1000,
            endMs: 2000,
          ),
          tripEvent(
            1,
            ['c.jpg'],
            folder: 'Oslo, Norway · May 2026',
            placeName: 'Oslo, Norway',
            startMs: 9000,
            endMs: 9000,
          ),
        ],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      container.read(wizardProvider.notifier).continueFromConfirm();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardTripsReview,
      );

      final wizard = container.read(wizardProvider.notifier);
      expect(wizard.tripResults.map((t) => t.id), [0, 1]);
      expect(wizard.tripResults.first.memberIds, ['a.jpg', 'b.jpg']);

      // Move b.jpg from trip 0 into trip 1.
      wizard.moveImagesToTrip(['b.jpg'], 1);
      expect(wizard.tripResults.firstWhere((t) => t.id == 0).memberIds, [
        'a.jpg',
      ]);
      expect(
        wizard.tripResults.firstWhere((t) => t.id == 1).memberIds,
        containsAll(<String>['b.jpg', 'c.jpg']),
      );

      // Pull a.jpg out of every trip -> trip 0 disappears, a.jpg is unassigned.
      wizard.moveImagesToTrip(['a.jpg'], null);
      expect(wizard.tripResults.map((t) => t.id), [1]);
      expect(wizard.unassignedTripImageIds, contains('a.jpg'));

      // Create a new trip from the unassigned photo.
      final newId = wizard.createTripFromImages(['a.jpg']);
      expect(wizard.tripResults.any((t) => t.id == newId), isTrue);
      expect(wizard.unassignedTripImageIds, isNot(contains('a.jpg')));
      // a.jpg was clustered under trip 0 ("Rome, Italy"), so the hand-made
      // trip borrows that place name rather than a bare "Trip · <month>".
      expect(
        wizard.tripResults.firstWhere((t) => t.id == newId).folder,
        contains('Rome, Italy'),
      );

      // Re-clustering sends the slider values and drops hand edits.
      wizard.rerunTripsPass(homeRadiusKm: 7, legRadiusKm: 40);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardTripsReview,
      );
      expect(client.lastTripsRequest!.homeRadiusKm, 7);
      expect(client.lastTripsRequest!.legRadiusKm, 40);
      expect(wizard.tripResults.map((t) => t.id), [0, 1]);
    });

    test('trips review: commit folder plan uses the geocoded slug', () async {
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.jpg'),
          scanImage('b.jpg'),
          scanComplete(images: 2),
        ],
        modelEvents: [modelReady()],
        similarEvents: const [],
        tripsEvents: [
          tripEvent(
            0,
            ['a.jpg'],
            folder: 'Rome, Italy · April 2026',
            folderSlug: 'rome-italy-2026-04',
            startMs: 1000,
            endMs: 2000,
          ),
          tripEvent(
            1,
            ['b.jpg'],
            folder: 'Oslo, Norway · May 2026',
            folderSlug: 'oslo-norway-2026-05',
            startMs: 9000,
            endMs: 9000,
          ),
        ],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      container.read(wizardProvider.notifier).continueFromConfirm();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardTripsReview,
      );

      final wizard = container.read(wizardProvider.notifier);
      final plan = wizard.commitFolderPlan();
      expect(plan['a.jpg'], 'rome-italy-2026-04');
      expect(plan['b.jpg'], 'oslo-norway-2026-05');

      // A user rename overrides the geocoded slug.
      wizard.renameTripFolder(0, 'Italy Trip');
      expect(wizard.commitFolderPlan()['a.jpg'], 'italy-trip');
    });

    test(
      'trips review: photos marked for deletion drop out of the panel',
      () async {
        final client = FakeKustaviClient(
          scanEvents: [
            scanImage('a.jpg'),
            scanImage('b.jpg'),
            scanImage('c.jpg'),
            scanComplete(images: 3),
          ],
          modelEvents: [modelReady()],
          similarEvents: const [],
          tripsEvents: [
            tripEvent(
              0,
              ['a.jpg', 'b.jpg'],
              folder: 'Rome, Italy · April 2026',
              startMs: 1000,
              endMs: 2000,
            ),
            tripEvent(
              1,
              ['c.jpg'],
              folder: 'Oslo, Norway · May 2026',
              startMs: 9000,
              endMs: 9000,
            ),
          ],
        );
        container = makeContainer(client);
        await reachConfirmFolder(container, client);
        container.read(wizardProvider.notifier).continueFromConfirm();
        await pumpUntil(
          container,
          () => container.read(wizardProvider).value is WizardTripsReview,
        );

        final wizard = container.read(wizardProvider.notifier);
        expect(wizard.tripResults.first.memberIds, ['a.jpg', 'b.jpg']);

        // Mark b.jpg for deletion: it leaves the trip and is not "unassigned".
        container.read(deletionPlanProvider.notifier).mark('b.jpg');
        expect(wizard.tripResults.firstWhere((t) => t.id == 0).memberIds, [
          'a.jpg',
        ]);
        expect(wizard.unassignedTripImageIds, isNot(contains('b.jpg')));

        // Marking every member removes the trip entirely.
        container.read(deletionPlanProvider.notifier).mark('c.jpg');
        expect(wizard.tripResults.map((t) => t.id), [0]);
      },
    );

    test('back from confirm discards results (fresh session → S0)', () async {
      final client = FakeKustaviClient(
        scanEvents: [scanImage('a.jpg'), scanComplete(images: 1)],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);

      container.read(wizardProvider.notifier).backFromConfirm();
      expect(container.read(wizardProvider).value, isA<WizardStart>());
      expect(container.read(wizardProvider.notifier).imageIds, isEmpty);
    });

    test('resetToStart clears the deletion plan (S13 start over)', () async {
      final client = FakeKustaviClient(
        scanEvents: [scanImage('a.jpg'), scanComplete(images: 1)],
      );
      container = makeContainer(client);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardStart,
      );
      container.read(deletionPlanProvider.notifier).mark('a.jpg');
      expect(
        container.read(deletionPlanProvider).explicitDeleted,
        contains('a.jpg'),
      );

      container.read(wizardProvider.notifier).resetToStart();
      expect(container.read(deletionPlanProvider).explicitDeleted, isEmpty);
    });
  });

  group('batches (a la carte passes)', () {
    /// a.jpg + b.jpg form the Rome trip, c.jpg the Oslo trip, d.jpg is in no
    /// trip: three batches.
    FakeKustaviClient threeBatchClient({
      List<pb.QualityEvent> qualityEvents = const [],
      List<pb.SimilarEvent> similarEvents = const [],
    }) {
      return FakeKustaviClient(
        scanEvents: [
          scanImage('a.jpg'),
          scanImage('b.jpg'),
          scanImage('c.jpg'),
          scanImage('d.jpg'),
          scanComplete(images: 4),
        ],
        tripsEvents: [
          tripEvent(0, ['a.jpg', 'b.jpg'], folder: 'Rome, Italy · April 2026'),
          tripEvent(1, ['c.jpg'], folder: 'Oslo, Norway · May 2026'),
        ],
        qualityEvents: qualityEvents,
        similarEvents: similarEvents,
      );
    }

    test('trips run first and their folders become the batches', () async {
      final client = threeBatchClient();
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      // No analysis pass ran to get here.
      await reachBatchMenu(container, client);

      expect(client.qualityPassCount, 0);
      final menu = container.read(wizardProvider).value as WizardBatchMenu;
      expect(menu.batches.map((b) => b.title), [
        'Oslo, Norway · May 2026',
        'Rome, Italy · April 2026',
        'Unassigned',
      ]);
      expect(menu.batches.map((b) => b.photoCount), [1, 2, 1]);
      expect(menu.selectedKey, menu.batches.first.key);
      expect(menu.markedCount, 0);
    });

    test('a pass runs on the selected batch only', () async {
      final client = threeBatchClient(qualityEvents: [qualityFlag('a.jpg')]);
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      await reachBatchMenu(container, client);
      final wizard = container.read(wizardProvider.notifier);

      wizard.selectBatch('Rome, Italy · April 2026');
      wizard.startBatchPass(WizardStep.quality);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardQualityReview,
      );

      expect(client.lastQualityScope, ['a.jpg', 'b.jpg']);
      expect(client.lastQualityBatchKey, 'Rome, Italy · April 2026');
      final review =
          container.read(wizardProvider).value as WizardQualityReview;
      expect(review.totalImages, 2); // the batch, not the whole library
      expect(review.flaggedCount, 1);
      expect(wizard.reviewScope, {'a.jpg', 'b.jpg'});

      // Done returns to the menu; only that batch shows the pass as done.
      wizard.closeBatchReview();
      final menu = container.read(wizardProvider).value as WizardBatchMenu;
      final status = {
        for (final b in menu.batches)
          b.title: b.passes[WizardStep.quality.index]!,
      };
      expect(status['Rome, Italy · April 2026']!.done, isTrue);
      expect(status['Rome, Italy · April 2026']!.flagged, 1);
      expect(status['Oslo, Norway · May 2026']!.done, isFalse);
      expect(status['Unassigned']!.done, isFalse);
      expect(wizard.reviewScope, isNull);
    });

    test('a batch review lists only that batch\'s flags', () async {
      final client = threeBatchClient(
        qualityEvents: [qualityFlag('a.jpg'), qualityFlag('c.jpg')],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      await reachBatchMenu(container, client);
      final wizard = container.read(wizardProvider.notifier);

      // The fake replays both flags for either run (the real back end would
      // only emit in-scope ones). Bulk actions and counts still honor scope.
      wizard.selectBatch('Rome, Italy · April 2026');
      wizard.startBatchPass(WizardStep.quality);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardQualityReview,
      );
      expect(
        (container.read(wizardProvider).value as WizardQualityReview)
            .flaggedCount,
        1,
      );
      wizard.keepAllQualityFlagged();
      final plan = container.read(deletionPlanProvider);
      expect(plan.explicitKept, {'a.jpg'});
    });

    test('passes can run in any order', () async {
      final client = threeBatchClient(
        similarEvents: [
          similarGroup(0, ['a.jpg', 'b.jpg'], 'a.jpg'),
        ],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      await reachBatchMenu(container, client);
      final wizard = container.read(wizardProvider.notifier);

      // Duplicates first, with no quality pass at all.
      wizard.selectBatch('Rome, Italy · April 2026');
      wizard.startBatchPass(WizardStep.duplicates);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardSimilarReview,
      );
      expect(client.lastSimilarScope, ['a.jpg', 'b.jpg']);
      expect(client.qualityPassCount, 0);
      expect(
        (container.read(wizardProvider).value as WizardSimilarReview)
            .groupCount,
        1,
      );
    });

    test(
      'run on all batches runs each batch once and returns to the menu',
      () async {
        final client = threeBatchClient();
        container = makeContainer(client);
        await reachConfirmFolder(container, client);
        await reachBatchMenu(container, client);
        final wizard = container.read(wizardProvider.notifier);

        wizard.startPassOnAllBatches(WizardStep.quality);
        await pumpUntil(
          container,
          () =>
              container.read(wizardProvider).value is WizardBatchMenu &&
              client.qualityPassCount == 3,
        );

        expect(client.qualityPassCount, 3);
        final menu = container.read(wizardProvider).value as WizardBatchMenu;
        expect(
          menu.batches.every((b) => b.passes[WizardStep.quality.index]!.done),
          isTrue,
        );
        // Nothing left to run now.
        wizard.startPassOnAllBatches(WizardStep.quality);
        expect(client.qualityPassCount, 3);
      },
    );

    test('cancelling a batch run returns to the menu', () async {
      final client = threeBatchClient();
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      await reachBatchMenu(container, client);
      final wizard = container.read(wizardProvider.notifier);

      wizard.startBatchPass(WizardStep.quality);
      expect(container.read(wizardProvider).value, isA<WizardQualityRunning>());
      wizard.cancelQuality();
      expect(container.read(wizardProvider).value, isA<WizardBatchMenu>());
      expect(wizard.reviewScope, isNull);
    });

    test('Edit trips returns to organize and rebuilds the batches', () async {
      final client = threeBatchClient();
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      await reachBatchMenu(container, client);
      final wizard = container.read(wizardProvider.notifier);

      wizard.reopenOrganize();
      expect(container.read(wizardProvider).value, isA<WizardTripsReview>());
      wizard.renameTripFolder(1, 'Norway');
      wizard.continueFromTrips();

      final menu = container.read(wizardProvider).value as WizardBatchMenu;
      expect(menu.batches.map((b) => b.title), contains('Norway'));
    });

    test(
      'Continue from the menu opens the commit summary and Back returns',
      () async {
        final client = threeBatchClient();
        container = makeContainer(client);
        await reachConfirmFolder(container, client);
        await reachBatchMenu(container, client);
        final wizard = container.read(wizardProvider.notifier);

        wizard.continueFromBatches();
        expect(
          container.read(wizardProvider).value,
          isA<WizardCommitSummary>(),
        );
        wizard.backFromCommitSummary();
        expect(container.read(wizardProvider).value, isA<WizardBatchMenu>());
      },
    );

    test('step indicator stages follow the flow', () async {
      final client = threeBatchClient();
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      expect(
        container.read(wizardProvider).value!.stageIndex,
        WizardStage.select.index,
      );
      container.read(wizardProvider.notifier).continueFromConfirm();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardTripsReview,
      );
      expect(
        container.read(wizardProvider).value!.stageIndex,
        WizardStage.organize.index,
      );
      container.read(wizardProvider.notifier).continueFromTrips();
      expect(
        container.read(wizardProvider).value!.stageIndex,
        WizardStage.review.index,
      );
    });
  });

  group('resume a saved session (S0-B)', () {
    test('a saved session routes select → WizardSessionRestore', () async {
      final client = FakeKustaviClient(
        inspectSessionResponse: inspectSession(imageCount: 42, resumeStep: 4),
      );
      container = makeContainer(client);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardStart,
      );

      container.read(wizardProvider.notifier).selectFolder('/photos');
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardSessionRestore,
      );
      final phase =
          container.read(wizardProvider).value as WizardSessionRestore;
      expect(phase.folder, '/photos');
      expect(phase.imageCount, 42);
      expect(phase.savedStepIndex, 4);
    });

    test(
      'resume with a finished pass restores it and runs only what is left',
      () async {
        // Saved at the video step: quality, similar and junk all finished; the
        // video pass had not. Only the video pass should run on resume.
        final client = FakeKustaviClient(
          inspectSessionResponse: inspectSession(imageCount: 3, resumeStep: 4),
          scanEvents: [
            scanImage('a.jpg'),
            scanImage('b.jpg'),
            scanImage('c.jpg'),
            scanComplete(images: 3, resumed: true, resumeStep: 4),
          ],
          sessionResults: sessionResults(
            resumeStep: 4,
            videoTotal: 1,
            qualityDone: true,
            similarDone: true,
            junkDone: true,
            videoDone: false,
            qualityFlags: [
              pb.QualityFlag()
                ..imageId = 'a.jpg'
                ..reasons.add(pb.QualityReason.BLURRY),
            ],
            junkFlags: [
              pb.JunkFlag()
                ..imageId = 'b.jpg'
                ..reason = 'screenshot',
            ],
            decisions: {'a.jpg': false, 'c.jpg': true},
          ),
          videoEvents: const [],
        );
        container = makeContainer(client);
        await pumpUntil(
          container,
          () => container.read(wizardProvider).value is WizardStart,
        );

        container.read(wizardProvider.notifier).selectFolder('/photos');
        await pumpUntil(
          container,
          () => container.read(wizardProvider).value is WizardSessionRestore,
        );
        container.read(wizardProvider.notifier).resumeSession();
        await pumpUntil(
          container,
          () => container.read(wizardProvider).value is WizardBatchMenu,
        );

        expect(client.lastScanRequest?.resume, isTrue);
        final wizard = container.read(wizardProvider.notifier);
        expect(wizard.imageIds, ['a.jpg', 'b.jpg', 'c.jpg']);
        expect(wizard.qualityFlags.keys, contains('a.jpg'));
        expect(wizard.junkFlags.keys, contains('b.jpg'));
        final plan = container.read(deletionPlanProvider);
        expect(plan.explicitKept, contains('a.jpg'));
        expect(plan.explicitDeleted, contains('c.jpg'));
        // Resume re-runs only the cheap trips pass, then lands on the batch
        // menu; no analysis pass is re-run.
        expect(client.qualityPassCount, 0);
        expect(client.lastVideoSkipIds, isEmpty);
        // Passes the saved session finished count as done for every batch.
        final menu = container.read(wizardProvider).value as WizardBatchMenu;
        final passes = menu.batches.single.passes;
        expect(passes[WizardStep.quality.index]!.done, isTrue);
        expect(passes[WizardStep.junk.index]!.done, isTrue);
        expect(passes[WizardStep.video.index]!.done, isFalse);
      },
    );

    test(
      'resume with every pass finished runs nothing and shows the review',
      () async {
        final client = FakeKustaviClient(
          inspectSessionResponse: inspectSession(imageCount: 2, resumeStep: 3),
          scanEvents: [
            scanImage('a.jpg'),
            scanImage('b.jpg'),
            scanComplete(images: 2, resumed: true, resumeStep: 3),
          ],
          sessionResults: sessionResults(
            resumeStep: 3,
            qualityDone: true,
            similarDone: true,
            junkDone: true,
            junkFlags: [
              pb.JunkFlag()
                ..imageId = 'a.jpg'
                ..reason = 'screenshot',
            ],
          ),
        );
        container = makeContainer(client);
        await pumpUntil(
          container,
          () => container.read(wizardProvider).value is WizardStart,
        );

        container.read(wizardProvider.notifier).selectFolder('/photos');
        await pumpUntil(
          container,
          () => container.read(wizardProvider).value is WizardSessionRestore,
        );
        container.read(wizardProvider.notifier).resumeSession();
        await pumpUntil(
          container,
          () => container.read(wizardProvider).value is WizardBatchMenu,
        );

        expect(client.qualityPassCount, 0);
        expect(client.lastVideoSkipIds, isEmpty); // video pass never ran
        expect(
          container.read(wizardProvider.notifier).junkFlags.keys,
          contains('a.jpg'),
        );
      },
    );

    test('resume restores per-batch pass completions', () async {
      final client = FakeKustaviClient(
        inspectSessionResponse: inspectSession(imageCount: 2, resumeStep: 1),
        scanEvents: [
          scanImage('a.jpg'),
          scanImage('b.jpg'),
          scanComplete(images: 2, resumed: true, resumeStep: 1),
        ],
        tripsEvents: [
          tripEvent(0, ['a.jpg'], folder: 'Rome'),
          tripEvent(1, ['b.jpg'], folder: 'Oslo'),
        ],
        sessionResults: sessionResults(
          resumeStep: 1,
          completedBatchPasses: ['1:Rome'],
        ),
      );
      container = makeContainer(client);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardStart,
      );
      container.read(wizardProvider.notifier).selectFolder('/photos');
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardSessionRestore,
      );
      container.read(wizardProvider.notifier).resumeSession();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardBatchMenu,
      );

      final menu = container.read(wizardProvider).value as WizardBatchMenu;
      final done = {
        for (final b in menu.batches)
          b.title: b.passes[WizardStep.quality.index]!.done,
      };
      expect(done, {'Oslo': false, 'Rome': true});
    });

    test('start fresh from the restore prompt scans normally', () async {
      final client = FakeKustaviClient(
        inspectSessionResponse: inspectSession(imageCount: 5, resumeStep: 3),
        scanEvents: [scanImage('a.jpg'), scanComplete(images: 1)],
      );
      container = makeContainer(client);
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardStart,
      );

      container.read(wizardProvider.notifier).selectFolder('/photos');
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardSessionRestore,
      );
      container.read(wizardProvider.notifier).startFreshFromRestore();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardConfirmFolder,
      );
      expect(client.lastScanRequest?.resume, isFalse);
    });
  });
}
