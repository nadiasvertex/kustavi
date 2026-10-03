import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:kustavi/src/backend/client_provider.dart';
import 'package:kustavi/src/state/phases.dart';
import 'package:kustavi/src/state/wizard.dart';
import 'package:kustavi/src/ui/wizard_shell.dart';

import '../helpers.dart';
import 'wizard_shell_test.dart' show makeContainer;

Future<String?> _pickPhotos() async => '/photos';

/// Drives the shell through scan and trips to the batch menu with two trip
/// batches: Rome (a, b) and Oslo (c).
Future<ProviderContainer> _openMenu(
  WidgetTester tester,
  FakeKustaviClient client,
) async {
  tester.view.physicalSize = const Size(1280, 800);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  final container = makeContainer(client);
  addTearDown(container.dispose);
  await tester.pumpWidget(
    UncontrolledProviderScope(
      container: container,
      child: const MaterialApp(home: WizardShell(pickDirectory: _pickPhotos)),
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
  expect(container.read(wizardProvider).value, isA<WizardBatchMenu>());
  return container;
}

/// A batch's row in the left-hand list (its title also heads the detail pane).
Finder _tile(String title) => find.widgetWithText(ListTile, title);

FakeKustaviClient _client() => FakeKustaviClient(
  scanEvents: [
    scanImage('a.jpg'),
    scanImage('b.jpg'),
    scanImage('c.jpg'),
    scanComplete(images: 3),
  ],
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
  qualityEvents: [qualityFlag('a.jpg')],
);

void main() {
  group('batch menu (§6.2)', () {
    testWidgets('lists every batch with its photo count and summary', (
      tester,
    ) async {
      await _openMenu(tester, _client());
      expect(
        find.text('2 batches · 0 of 3 photos marked for deletion'),
        findsOneWidget,
      );
      expect(_tile('Rome, Italy · April 2026'), findsOneWidget);
      expect(_tile('Oslo, Norway · May 2026'), findsOneWidget);
      expect(find.text('2 photos'), findsOneWidget);
      expect(find.text('1 photos'), findsOneWidget);
    });

    testWidgets('shows a card for each pass, none run yet', (tester) async {
      await _openMenu(tester, _client());
      for (final label in ['Quality', 'Duplicates', 'Junk']) {
        expect(find.text(label), findsOneWidget, reason: label);
      }
      expect(find.text('Not run'), findsNWidgets(3));
      // No videos in these batches, so no video card.
      expect(find.text('Video'), findsNothing);
      expect(find.text('Run'), findsNWidgets(3));
      expect(find.text('Run on all batches'), findsNWidgets(3));
    });

    testWidgets('tapping a batch selects it', (tester) async {
      final container = await _openMenu(tester, _client());
      String selectedTitle() {
        final phase = container.read(wizardProvider).value as WizardBatchMenu;
        return phase.batches
            .firstWhere((b) => b.key == phase.selectedKey)
            .title;
      }

      await tester.tap(_tile('Oslo, Norway · May 2026'));
      await tester.pump();
      expect(selectedTitle(), 'Oslo, Norway · May 2026');
      await tester.tap(_tile('Rome, Italy · April 2026'));
      await tester.pump();
      expect(selectedTitle(), 'Rome, Italy · April 2026');
    });

    testWidgets('a finished pass offers Review and Run again', (tester) async {
      final container = await _openMenu(tester, _client());
      // Rome holds the flagged photo.
      await tester.tap(_tile('Rome, Italy · April 2026'));
      await tester.pump();
      await tester.tap(find.text('Run').first);
      await tester.pump();
      await tester.pump();
      await tester.tap(find.text('Done'));
      await tester.pump();

      expect(container.read(wizardProvider).value, isA<WizardBatchMenu>());
      expect(find.text('Done · 1 flagged'), findsOneWidget);
      final review = find.widgetWithText(OutlinedButton, 'Review');
      expect(review, findsOneWidget);
      expect(find.text('Run again'), findsOneWidget);
      // The tile shows how many passes the batch has finished.
      expect(find.text('1 done'), findsOneWidget);

      await tester.tap(review);
      await tester.pump();
      expect(container.read(wizardProvider).value, isA<WizardQualityReview>());
    });
  });
}
