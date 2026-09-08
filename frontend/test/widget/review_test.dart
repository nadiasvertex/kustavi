import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:kustavi/src/backend/client_provider.dart';
import 'package:kustavi/src/backend/process.dart';
import 'package:kustavi/src/ui/widgets/detail_view.dart';
import 'package:kustavi/src/ui/widgets/image_cell.dart';
import 'package:kustavi/src/ui/wizard_shell.dart';

import '../helpers.dart';

ProviderContainer _makeContainer(FakeKustaviClient client) {
  return ProviderContainer(
    overrides: [
      kustaviClientProvider.overrideWith((ref) => client),
      backendProcessProvider.overrideWith(InactiveBackendProcess.new),
    ],
  );
}

Future<String?> _pickPhotos() async => '/photos';

/// Drives the shell from folder-pick into the S4 quality review.
Future<void> _toQualityReview(WidgetTester tester, ProviderContainer c) async {
  // Roomy surface so grid cells sit fully inside their flex-limited viewport
  // and taps land on them.
  await tester.binding.setSurfaceSize(const Size(1400, 1600));
  addTearDown(() => tester.binding.setSurfaceSize(null));
  await tester.pumpWidget(
    UncontrolledProviderScope(
      container: c,
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
}

void main() {
  group('FlaggedReview keep / delete sections (§6.2)', () {
    testWidgets('every flagged photo starts in the delete panel', (
      tester,
    ) async {
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.jpg'),
          scanImage('b.jpg'),
          scanComplete(images: 2),
        ],
        qualityEvents: [qualityFlag('a.jpg'), qualityFlag('b.jpg')],
      );
      final container = _makeContainer(client);
      addTearDown(container.dispose);

      await _toQualityReview(tester, container);

      expect(find.text('Keeping 0'), findsOneWidget);
      expect(find.text('Deleting 2'), findsOneWidget);
      expect(find.text('Tap a photo below to keep it'), findsOneWidget);
      expect(find.byType(ImageCell), findsNWidgets(2));
    });

    testWidgets('tapping a cell moves it between the two sections', (
      tester,
    ) async {
      final client = FakeKustaviClient(
        scanEvents: [
          scanImage('a.jpg'),
          scanImage('b.jpg'),
          scanComplete(images: 2),
        ],
        qualityEvents: [qualityFlag('a.jpg'), qualityFlag('b.jpg')],
      );
      final container = _makeContainer(client);
      addTearDown(container.dispose);

      await _toQualityReview(tester, container);

      // Delete panel -> keep section.
      await tester.tap(find.byType(ImageCell).first);
      await tester.pump();
      expect(find.text('Keeping 1'), findsOneWidget);
      expect(find.text('Deleting 1'), findsOneWidget);
      expect(find.text('Tap a photo below to keep it'), findsNothing);

      // Keep section -> back to the delete panel.
      await tester.tap(find.byType(ImageCell).first);
      await tester.pump();
      expect(find.text('Keeping 0'), findsOneWidget);
      expect(find.text('Deleting 2'), findsOneWidget);
    });

    testWidgets('collapsing the delete panel hides its grid', (tester) async {
      final client = FakeKustaviClient(
        scanEvents: [scanImage('a.jpg'), scanComplete(images: 1)],
        qualityEvents: [qualityFlag('a.jpg')],
      );
      final container = _makeContainer(client);
      addTearDown(container.dispose);

      await _toQualityReview(tester, container);

      expect(find.byType(ImageCell), findsOneWidget);
      await tester.tap(find.text('Deleting 1'));
      await tester.pump();
      expect(find.byType(ImageCell), findsNothing);
    });

    testWidgets('the corner button opens detail without moving the cell', (
      tester,
    ) async {
      final client = FakeKustaviClient(
        scanEvents: [scanImage('a.jpg'), scanComplete(images: 1)],
        qualityEvents: [qualityFlag('a.jpg')],
      );
      final container = _makeContainer(client);
      addTearDown(container.dispose);

      await _toQualityReview(tester, container);

      await tester.tap(find.byIcon(Icons.open_in_full));
      await tester.pumpAndSettle();

      expect(find.byType(DetailView), findsOneWidget);
      // Still marked for deletion — opening detail did not move it.
      expect(find.text('Marked for deletion'), findsOneWidget);
    });
  });
}
