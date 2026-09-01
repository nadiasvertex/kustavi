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

    test('continue from similar → junk pass → junk review', () async {
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
      container.read(wizardProvider.notifier).continueFromConfirm();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardQualityReview,
      );
      // The quality-flagged image stays marked for deletion by default.
      container.read(wizardProvider.notifier).continueFromQuality();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardSimilarReview,
      );
      await pumpUntil(
        container,
        () => container.read(modelStatusProvider).value is ModelPrepReady,
      );
      container.read(wizardProvider.notifier).continueFromSimilar();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardJunkReview,
      );
      expect(client.lastJunkSkipIds, contains('a.jpg'));
      expect(client.lastJunkSkipIds, isNot(contains('b.jpg')));
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

      container.read(wizardProvider.notifier).continueFromTrips();
      await pumpUntil(
        container,
        () => container.read(wizardProvider).value is WizardCommitSummary,
      );
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
      wizard.continueFromConfirm();
      await pumpUntil(container,
          () => container.read(wizardProvider).value is WizardQualityReview);
      wizard.continueFromQuality();
      await pumpUntil(container,
          () => container.read(wizardProvider).value is WizardSimilarReview);
      await pumpUntil(container,
          () => container.read(modelStatusProvider).value is ModelPrepReady);
      wizard.continueFromSimilar();
      await pumpUntil(container,
          () => container.read(wizardProvider).value is WizardJunkReview);
      wizard.continueFromJunk();
      await pumpUntil(container,
          () => container.read(wizardProvider).value is WizardTripsReview);
      wizard.continueFromTrips();
      await pumpUntil(container,
          () => container.read(wizardProvider).value is WizardCommitSummary);

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
        scanEvents: [
          scanImage('a.jpg'),
          scanComplete(images: 1),
        ],
        modelEvents: [modelReady()],
        similarEvents: const [],
        commitEvents: [commitProgress(done: 0, total: 1)],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      final wizard = container.read(wizardProvider.notifier);
      wizard.continueFromConfirm();
      await pumpUntil(container,
          () => container.read(wizardProvider).value is WizardQualityReview);
      wizard.continueFromQuality();
      await pumpUntil(container,
          () => container.read(wizardProvider).value is WizardSimilarReview);
      await pumpUntil(container,
          () => container.read(modelStatusProvider).value is ModelPrepReady);
      wizard.continueFromSimilar();
      await pumpUntil(container,
          () => container.read(wizardProvider).value is WizardJunkReview);
      wizard.continueFromJunk();
      await pumpUntil(container,
          () => container.read(wizardProvider).value is WizardTripsReview);
      wizard.continueFromTrips();
      await pumpUntil(container,
          () => container.read(wizardProvider).value is WizardCommitSummary);

      wizard.startCommit();
      wizard.cancelCommit();
      expect(
        container.read(wizardProvider).value,
        isA<WizardCommitSummary>(),
      );
    });

    test('trips review: move photos between trips, create, unassign, rerun',
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
          tripEvent(0, ['a.jpg', 'b.jpg'],
              folder: 'Rome, Italy · April 2026',
              startMs: 1000,
              endMs: 2000),
          tripEvent(1, ['c.jpg'],
              folder: 'Oslo, Norway · May 2026',
              startMs: 9000,
              endMs: 9000),
        ],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      container.read(wizardProvider.notifier).continueFromConfirm();
      await pumpUntil(container,
          () => container.read(wizardProvider).value is WizardQualityReview);
      container.read(wizardProvider.notifier).continueFromQuality();
      await pumpUntil(container,
          () => container.read(wizardProvider).value is WizardSimilarReview);
      await pumpUntil(container,
          () => container.read(modelStatusProvider).value is ModelPrepReady);
      container.read(wizardProvider.notifier).continueFromSimilar();
      await pumpUntil(container,
          () => container.read(wizardProvider).value is WizardJunkReview);
      container.read(wizardProvider.notifier).continueFromJunk();
      await pumpUntil(container,
          () => container.read(wizardProvider).value is WizardTripsReview);

      final wizard = container.read(wizardProvider.notifier);
      expect(wizard.tripResults.map((t) => t.id), [0, 1]);
      expect(wizard.tripResults.first.memberIds, ['a.jpg', 'b.jpg']);

      // Move b.jpg from trip 0 into trip 1.
      wizard.moveImagesToTrip(['b.jpg'], 1);
      expect(wizard.tripResults.firstWhere((t) => t.id == 0).memberIds,
          ['a.jpg']);
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

      // Re-clustering sends the slider values and drops hand edits.
      wizard.rerunTripsPass(homeRadiusKm: 7, legRadiusKm: 40);
      await pumpUntil(container,
          () => container.read(wizardProvider).value is WizardTripsReview);
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
          tripEvent(0, ['a.jpg'],
              folder: 'Rome, Italy · April 2026',
              folderSlug: 'rome-italy-2026-04',
              startMs: 1000,
              endMs: 2000),
          tripEvent(1, ['b.jpg'],
              folder: 'Oslo, Norway · May 2026',
              folderSlug: 'oslo-norway-2026-05',
              startMs: 9000,
              endMs: 9000),
        ],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      container.read(wizardProvider.notifier).continueFromConfirm();
      await pumpUntil(container,
          () => container.read(wizardProvider).value is WizardQualityReview);
      container.read(wizardProvider.notifier).continueFromQuality();
      await pumpUntil(container,
          () => container.read(wizardProvider).value is WizardSimilarReview);
      await pumpUntil(container,
          () => container.read(modelStatusProvider).value is ModelPrepReady);
      container.read(wizardProvider.notifier).continueFromSimilar();
      await pumpUntil(container,
          () => container.read(wizardProvider).value is WizardJunkReview);
      container.read(wizardProvider.notifier).continueFromJunk();
      await pumpUntil(container,
          () => container.read(wizardProvider).value is WizardTripsReview);

      final wizard = container.read(wizardProvider.notifier);
      final plan = wizard.commitFolderPlan();
      expect(plan['a.jpg'], 'rome-italy-2026-04');
      expect(plan['b.jpg'], 'oslo-norway-2026-05');

      // A user rename overrides the geocoded slug.
      wizard.renameTripFolder(0, 'Italy Trip');
      expect(wizard.commitFolderPlan()['a.jpg'], 'italy-trip');
    });

    test('trips review: photos marked for deletion drop out of the panel',
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
          tripEvent(0, ['a.jpg', 'b.jpg'],
              folder: 'Rome, Italy · April 2026', startMs: 1000, endMs: 2000),
          tripEvent(1, ['c.jpg'],
              folder: 'Oslo, Norway · May 2026', startMs: 9000, endMs: 9000),
        ],
      );
      container = makeContainer(client);
      await reachConfirmFolder(container, client);
      container.read(wizardProvider.notifier).continueFromConfirm();
      await pumpUntil(container,
          () => container.read(wizardProvider).value is WizardQualityReview);
      container.read(wizardProvider.notifier).continueFromQuality();
      await pumpUntil(container,
          () => container.read(wizardProvider).value is WizardSimilarReview);
      await pumpUntil(container,
          () => container.read(modelStatusProvider).value is ModelPrepReady);
      container.read(wizardProvider.notifier).continueFromSimilar();
      await pumpUntil(container,
          () => container.read(wizardProvider).value is WizardJunkReview);
      container.read(wizardProvider.notifier).continueFromJunk();
      await pumpUntil(container,
          () => container.read(wizardProvider).value is WizardTripsReview);

      final wizard = container.read(wizardProvider.notifier);
      expect(wizard.tripResults.first.memberIds, ['a.jpg', 'b.jpg']);

      // Mark b.jpg for deletion: it leaves the trip and is not "unassigned".
      container.read(deletionPlanProvider.notifier).mark('b.jpg');
      expect(wizard.tripResults.firstWhere((t) => t.id == 0).memberIds,
          ['a.jpg']);
      expect(wizard.unassignedTripImageIds, isNot(contains('b.jpg')));

      // Marking every member removes the trip entirely.
      container.read(deletionPlanProvider.notifier).mark('c.jpg');
      expect(wizard.tripResults.map((t) => t.id), [0]);
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
