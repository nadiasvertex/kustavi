import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:kustavi/src/backend/client_provider.dart';
import 'package:kustavi/src/backend/process.dart';
import 'package:kustavi/src/state/phases.dart';
import 'package:kustavi/src/state/wizard.dart';
import 'package:kustavi/src/ui/wizard_shell.dart';

import '../helpers.dart';

ProviderContainer makeContainer(FakeKustaviClient client) {
  return ProviderContainer(
    overrides: [
      kustaviClientProvider.overrideWith((ref) => client),
      // Never launch the real back end from a widget test.
      backendProcessProvider.overrideWith(() => InactiveBackendProcess()),
    ],
  );
}

Future<String?> _pickPhotos() async => '/photos';

void main() {
  group('wizard shell end-to-end (§6)', () {
    testWidgets('step indicator shows the six steps', (tester) async {
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
      for (final label in [
        'Select',
        'Quality',
        'Junk',
        'Duplicates',
        'Trips',
        'Copy',
      ]) {
        expect(find.text(label), findsOneWidget, reason: label);
      }
    });

    testWidgets('S0 → S1 → S2 flow with the action bar', (tester) async {
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

      // Continue runs the quality pass (S3) into S4.
      await tester.tap(find.text('Continue'));
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
    });

    testWidgets('quality sliders update and enable rerun', (tester) async {
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.jpg'),
          scanImage('b.jpg'),
          scanComplete(images: 2),
        ],
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
      await tester.tap(find.text('Select folder…'));
      await tester.pump();
      await tester.pump();
      await tester.tap(find.text('Continue'));
      await tester.pump();
      await tester.pump();

      expect(find.text('1 of 2 images flagged'), findsOneWidget);
      expect(find.text('Rerun pass'), findsNothing);

      await tester.drag(find.byType(Slider).first, const Offset(150, 0));
      await tester.pump();

      final value = tester.widget<Slider>(find.byType(Slider).first).value;
      expect(value, isNot(100.0));
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

    testWidgets('a step error shows the error screen with [Back]',
        (tester) async {
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

      expect(find.text('Processing error'), findsOneWidget);
      expect(find.text('quality exploded'), findsOneWidget);

      await tester.tap(find.text('Back'));
      await tester.pump();
      expect(
        container.read(wizardProvider).value,
        isA<WizardConfirmFolder>(),
      );
    });
  });
}
