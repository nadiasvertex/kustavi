import 'dart:ui' show AppExitResponse;

import 'package:fixnum/fixnum.dart';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:kustavi/src/backend/client_provider.dart';
import 'package:kustavi/src/backend/process.dart';
import 'package:kustavi/src/generated/kustavi/service.pb.dart';
import 'package:kustavi/src/state/phases.dart';
import 'package:kustavi/src/state/wizard.dart';
import 'package:kustavi/src/ui/wizard_shell.dart';

import '../helpers.dart';

ProviderContainer makeContainer(
  FakeKustaviClient client, {
  BackendProcess Function()? backend,
}) {
  return ProviderContainer(
    overrides: [
      kustaviClientProvider.overrideWith((ref) => client),
      // Never launch the real back end from a widget test.
      backendProcessProvider.overrideWith(backend ?? InactiveBackendProcess.new),
    ],
  );
}

/// Records whether a clean shutdown was requested.
class RecordingBackendProcess extends InactiveBackendProcess {
  int quitCalls = 0;

  @override
  Future<void> quit() async => quitCalls++;
}

Future<String?> _pickPhotos() async => '/photos';

void main() {
  group('wizard shell end-to-end (§6)', () {
    testWidgets('step indicator shows every step', (tester) async {
      final container = makeContainer(FakeKustaviClient());
      addTearDown(container.dispose);
      await tester.pumpWidget(
        UncontrolledProviderScope(
          container: container,
          child: const MaterialApp(
            home: WizardShell(pickDirectory: _pickPhotos),
          ),
        ),
      );
      await tester.pump();
      for (final label in ['Select', 'Organize', 'Review', 'Copy']) {
        expect(find.text(label), findsOneWidget, reason: label);
      }
    });

    testWidgets('S0 → S1 → S2 flow with the action bar', (tester) async {
      // Default app window: the batch menu needs more than the 800×600 test
      // surface for its pass cards to sit on screen.
      tester.view.physicalSize = const Size(1280, 800);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.reset);
      final client = FakeKustaviClient(
        // The scan stream stays open so S1 is observable; the test pushes
        // the completion event once the scanning screen is asserted.
        scanEvents: [scanImage('a.jpg'), scanImage('b.jpg')],
        scanStreamStaysOpen: true,
        qualityEvents: [qualityFlag('a.jpg')],
      );
      final container = makeContainer(client);
      addTearDown(container.dispose);
      await tester.pumpWidget(
        UncontrolledProviderScope(
          container: container,
          child: const MaterialApp(
            home: WizardShell(pickDirectory: _pickPhotos),
          ),
        ),
      );
      await tester.pump();

      // S0: no action bar, select the folder.
      expect(find.text('Exit app'), findsNothing);
      await tester.tap(find.text('Select folder…'));
      await tester.pump();

      // S1: scanning with a [Cancel] in the action bar.
      expect(find.text('Scanning /photos'), findsOneWidget);
      expect(find.text('Cancel'), findsOneWidget);

      // The scan completes into S2.
      client.pushScanEvent(scanComplete(images: 2));
      client.closeScanStream();
      await tester.pump();
      expect(find.text('2 images in /photos'), findsOneWidget);
      expect(find.text('Back'), findsOneWidget);
      expect(find.text('Continue'), findsOneWidget);

      // Continue runs the trips pass (Organize), then opens the batch menu.
      await tester.tap(find.text('Continue'));
      await tester.pump();
      await tester.pump();
      expect(container.read(wizardProvider).value, isA<WizardTripsReview>());
      await tester.tap(find.text('Continue'));
      await tester.pump();
      expect(container.read(wizardProvider).value, isA<WizardBatchMenu>());
      expect(find.text('Unassigned'), findsWidgets);

      // The menu runs whichever pass the user picks, here quality.
      await tester.tap(find.text('Run').first);
      await tester.pump();
      expect(
        container.read(wizardProvider).value,
        anyOf(isA<WizardQualityRunning>(), isA<WizardQualityReview>()),
      );
      await tester.pump();
      expect(
        find.text('1 of 2 images flagged'),
        findsOneWidget,
      );
      // The review closes back to the menu.
      await tester.tap(find.text('Done'));
      await tester.pump();
      expect(container.read(wizardProvider).value, isA<WizardBatchMenu>());

      // Continue opens the final review of everything marked for deletion.
      await tester.tap(find.text('Continue'));
      await tester.pump();
      expect(container.read(wizardProvider).value, isA<WizardDeletionReview>());
      expect(find.text('1 photos marked for deletion'), findsOneWidget);
      await tester.tap(find.text('Continue'));
      await tester.pump();
      expect(container.read(wizardProvider).value, isA<WizardCommitSummary>());

      // The space estimate arrives and enables [Copy].
      await tester.pump(Duration.zero);
      await tester.pump();
      expect(client.lastEstimateRequest?.mergeExisting, isTrue);
      expect(
        tester.widget<FilledButton>(find.widgetWithText(FilledButton, 'Copy'))
            .onPressed,
        isNotNull,
      );
    });

    testWidgets('Copy stays disabled when the destination is too small', (
      tester,
    ) async {
      tester.view.physicalSize = const Size(1280, 800);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.reset);
      final client = FakeKustaviClient(
        scanEvents: [scanImage('a.jpg'), scanComplete(images: 1)],
        estimateCommitResponse: EstimateCommitResponse(
          totalBytes: Int64(10),
          newBytes: Int64(10),
          freeBytesKnown: true,
          freeBytes: Int64(1),
          fits: false,
        ),
      );
      final container = makeContainer(client);
      addTearDown(container.dispose);
      await tester.pumpWidget(
        UncontrolledProviderScope(
          container: container,
          child: const MaterialApp(
            home: WizardShell(pickDirectory: _pickPhotos),
          ),
        ),
      );
      await tester.pump();
      await tester.tap(find.text('Select folder…'));
      await tester.pump();
      await tester.pump();
      await tester.tap(find.text('Continue'));
      await tester.pump();
      await tester.pump();
      await tester.tap(find.text('Continue'));
      await tester.pump();
      await tester.tap(find.text('Continue'));
      await tester.pump();
      await tester.tap(find.text('Continue'));
      await tester.pump();
      expect(container.read(wizardProvider).value, isA<WizardCommitSummary>());
      await tester.pump(Duration.zero);
      await tester.pump();

      expect(find.textContaining('Not enough space'), findsOneWidget);
      expect(
        tester.widget<FilledButton>(find.widgetWithText(FilledButton, 'Copy'))
            .onPressed,
        isNull,
      );
    });

    testWidgets('quality sliders update and enable rerun', (tester) async {
      // Default app window; the slider panel needs more than the 800×600 test
      // surface once the direction hints are shown.
      tester.view.physicalSize = const Size(1280, 800);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.reset);
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.jpg'),
          scanImage('b.jpg'),
          scanComplete(images: 2),
        ],
        qualityEvents: [qualityFlag('a.jpg')],
      )..previewResponse = (PreviewQualityThresholdsResponse()..flagged = 2);
      final container = makeContainer(client);
      addTearDown(container.dispose);
      await tester.pumpWidget(
        UncontrolledProviderScope(
          container: container,
          child: const MaterialApp(
            home: WizardShell(pickDirectory: _pickPhotos),
          ),
        ),
      );
      await tester.pump();
      await tester.tap(find.text('Select folder…'));
      await tester.pump();
      await tester.pump();
      await tester.tap(find.text('Continue'));
      await tester.pump();
      await tester.pump();
      await tester.tap(find.text('Continue'));
      await tester.pump();
      await tester.tap(find.text('Run').first);
      await tester.pump();
      await tester.pump();

      expect(find.text('1 of 2 images flagged'), findsOneWidget);
      expect(find.text('Rerun pass'), findsNothing);

      // The threshold panel starts collapsed so the grid gets full height.
      expect(find.byType(Slider), findsNothing);
      await tester.tap(find.text('Quality thresholds'));
      await tester.pump();

      await tester.drag(find.byType(Slider).first, const Offset(150, 0));
      await tester.pump();

      final value = tester.widget<Slider>(find.byType(Slider).first).value;
      expect(value, isNot(100.0));
      // The back end's preview count appears next to the last-run count.
      await tester.pump();
      expect(find.textContaining('These settings flag 2 of 2'), findsOneWidget);
      // The value label follows the new threshold.
      expect(
        find.text(
          value >= 100 && value == value.roundToDouble()
              ? value.toInt().toString()
              : value.toStringAsFixed(1),
        ),
        findsOneWidget,
      );
      // The adjusted threshold enables the rerun button.
      expect(find.text('Rerun pass'), findsOneWidget);
    });

    testWidgets('zero images → no-images screen with actions',
        (tester) async {
      final client = FakeKustaviClient(scanEvents: [scanComplete(images: 0)]);
      final container = makeContainer(client);
      addTearDown(container.dispose);
      await tester.pumpWidget(
        UncontrolledProviderScope(
          container: container,
          child: const MaterialApp(
            home: WizardShell(pickDirectory: _pickPhotos),
          ),
        ),
      );
      await tester.pump();
      await tester.tap(find.text('Select folder…'));
      await tester.pump();
      await tester.pump();

      expect(
        find.text('No images found in /photos'),
        findsOneWidget,
      );
      expect(find.text('Choose another folder'), findsOneWidget);
      expect(find.text('Exit app'), findsOneWidget);

      await tester.tap(find.text('Choose another folder'));
      await tester.pump();
      expect(
        container.read(wizardProvider).value,
        isA<WizardStart>(),
      );
    });

    testWidgets('an OS window-close request shuts the back end down cleanly',
        (tester) async {
      final backend = RecordingBackendProcess();
      final container = makeContainer(
        FakeKustaviClient(),
        backend: () => backend,
      );
      addTearDown(container.dispose);
      await tester.pumpWidget(
        UncontrolledProviderScope(
          container: container,
          child: const MaterialApp(
            home: WizardShell(pickDirectory: _pickPhotos),
          ),
        ),
      );
      await tester.pump();

      final response =
          await tester.binding.handleRequestAppExit();

      expect(response, AppExitResponse.exit);
      expect(backend.quitCalls, 1);
    });

    testWidgets('a step error shows the error screen with [Back]',
        (tester) async {
      // Default app window: the batch menu needs more than the 800×600 test
      // surface for its pass cards to sit on screen.
      tester.view.physicalSize = const Size(1280, 800);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.reset);
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.jpg'),
          scanComplete(images: 1),
        ],
        qualityError: rpcBoom('quality exploded'),
      );
      final container = makeContainer(client);
      addTearDown(container.dispose);
      await tester.pumpWidget(
        UncontrolledProviderScope(
          container: container,
          child: const MaterialApp(
            home: WizardShell(pickDirectory: _pickPhotos),
          ),
        ),
      );
      await tester.pump();
      await tester.tap(find.text('Select folder…'));
      await tester.pump();
      await tester.pump();
      await tester.tap(find.text('Continue'));
      await tester.pump();
      await tester.pump();
      await tester.tap(find.text('Continue'));
      await tester.pump();
      await tester.tap(find.text('Run').first);
      await tester.pump();

      expect(find.text('Processing error'), findsOneWidget);
      expect(find.text('quality exploded'), findsOneWidget);

      await tester.tap(find.text('Back'));
      await tester.pump();
      expect(container.read(wizardProvider).value, isA<WizardBatchMenu>());
    });
  });
}
