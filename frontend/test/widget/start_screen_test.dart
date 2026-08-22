import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:kustavi/src/backend/client_provider.dart';
import 'package:kustavi/src/state/phases.dart';
import 'package:kustavi/src/state/wizard.dart';
import 'package:kustavi/src/ui/start.dart';

import '../helpers.dart';

ProviderContainer makeContainer(FakeKustaviClient client) {
  return ProviderContainer(
    overrides: [kustaviClientProvider.overrideWith((ref) => client)],
  );
}

Future<String?> _rejectPicker() async => null;

void main() {
  group('S0 start screen (§6.2)', () {
    testWidgets('shows the title and the select-folder action',
        (tester) async {
      final container = makeContainer(FakeKustaviClient());
      addTearDown(container.dispose);
      await tester.pumpWidget(
        UncontrolledProviderScope(
          container: container,
          child: const MaterialApp(
            home: Scaffold(body: StartScreen(onPickDirectory: _rejectPicker)),
          ),
        ),
      );
      await tester.pump();
      expect(find.text('Kustavi'), findsOneWidget);
      expect(find.text('Select folder…'), findsOneWidget);
      // No model card: the fake reports no download in progress.
      expect(find.textContaining('Preparing vision model'), findsNothing);
    });

    testWidgets('shows the model prep card while downloading',
        (tester) async {
      final container = makeContainer(
        FakeKustaviClient(
          modelEvents: [
            modelProgress(doneBytes: 600000000, totalBytes: 1200000000),
          ],
          modelStreamStaysOpen: true,
        ),
      );
      addTearDown(container.dispose);
      await tester.pumpWidget(
        UncontrolledProviderScope(
          container: container,
          child: const MaterialApp(
            home: Scaffold(
              body: StartScreen(onPickDirectory: _rejectPicker),
            ),
          ),
        ),
      );
      await tester.pump();
      expect(find.textContaining('Preparing vision model'), findsOneWidget);
      expect(find.textContaining('50%'), findsOneWidget);
      expect(find.textContaining('0.6 GB / 1.2 GB'), findsOneWidget);
    });

    testWidgets('hides the model card once ready', (tester) async {
      final container =
          makeContainer(FakeKustaviClient(modelEvents: [modelReady()]));
      addTearDown(container.dispose);
      await tester.pumpWidget(
        UncontrolledProviderScope(
          container: container,
          child: const MaterialApp(
            home: Scaffold(
              body: StartScreen(onPickDirectory: _rejectPicker),
            ),
          ),
        ),
      );
      await tester.pump();
      expect(find.textContaining('Preparing vision model'), findsNothing);
    });

    testWidgets('selecting a folder starts the scan (S0 → S1)',
        (tester) async {
      final container = makeContainer(FakeKustaviClient());
      addTearDown(container.dispose);
      var picked = false;
      await tester.pumpWidget(
        UncontrolledProviderScope(
          container: container,
          child: MaterialApp(
            home: Scaffold(
              body: StartScreen(
                onPickDirectory: () async {
                  picked = true;
                  return '/photos';
                },
              ),
            ),
          ),
        ),
      );
      await tester.pump();
      // The wizard's async build must settle before selectFolder accepts
      // input (its guard no-ops while the provider is still loading).
      await container.read(wizardProvider.future);
      await tester.pump();

      await tester.tap(find.text('Select folder…'));
      await tester.pump();
      expect(picked, isTrue);
      expect(
        container.read(wizardProvider).value,
        isA<WizardScanning>(),
      );
    });

    testWidgets('a cancelled picker keeps the start screen', (tester) async {
      final container = makeContainer(FakeKustaviClient());
      addTearDown(container.dispose);
      await tester.pumpWidget(
        UncontrolledProviderScope(
          container: container,
          child: const MaterialApp(
            home: Scaffold(
              body: StartScreen(onPickDirectory: _rejectPicker),
            ),
          ),
        ),
      );
      await tester.pump();
      await container.read(wizardProvider.future);
      await tester.pump();

      await tester.tap(find.text('Select folder…'));
      await tester.pump();
      expect(
        container.read(wizardProvider).value,
        isA<WizardStart>(),
      );
    });
  });
}
