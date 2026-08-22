import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/material.dart' hide ImageInfo;
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:kustavi/src/state/decisions.dart';
import 'package:kustavi/src/state/domain.dart';
import 'package:kustavi/src/ui/widgets/detail_view.dart';

/// 1x1 transparent PNG, decodable by the platform codec.
final Uint8List kPx = base64Decode(
  'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJ'
  'AAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==',
);

void main() {
  late Directory tempDir;

  setUp(() async {
    tempDir = await Directory.systemTemp.createTemp('kustavi_detail_test');
  });

  tearDown(() async {
    await tempDir.delete(recursive: true);
  });

  ImageInfo makeImage() {
    final working = File('${tempDir.path}/working.jpg')..writeAsBytesSync(kPx);
    final master = File('${tempDir.path}/master.jpg')..writeAsBytesSync(kPx);
    return ImageInfo(
      id: 'a.jpg',
      path: master.path,
      name: 'a.jpg',
      sizeBytes: 8000000,
      width: 1,
      height: 1,
      workingImagePath: working.path,
    );
  }

  group('detail view (§7.2, §12 progressive swap)', () {
    testWidgets('swaps the working preview for the decoded master',
        (tester) async {
      final image = makeImage();
      final container = ProviderContainer();
      addTearDown(container.dispose);
      await tester.pumpWidget(
        UncontrolledProviderScope(
          container: container,
          child: MaterialApp(
            home: Scaffold(
              body: DetailView(image: image, canToggleDeletion: false),
            ),
          ),
        ),
      );
      await tester.pump();

      // The cached 768px working preview is visible immediately…
      expect(find.byKey(const ValueKey('working')), findsOneWidget);
      expect(find.byKey(const ValueKey('master')), findsNothing);

      // …and is replaced by the full-resolution master once decoded.
      await tester.pumpAndSettle();
      expect(find.byKey(const ValueKey('master')), findsOneWidget);
      expect(find.byKey(const ValueKey('working')), findsNothing);
    });

    testWidgets('metadata panel shows name, size, and dimensions',
        (tester) async {
      final image = makeImage();
      final container = ProviderContainer();
      addTearDown(container.dispose);
      await tester.pumpWidget(
        UncontrolledProviderScope(
          container: container,
          child: MaterialApp(
            home: Scaffold(
              body: DetailView(image: image, canToggleDeletion: false),
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(find.text('a.jpg'), findsWidgets);
      expect(find.textContaining('8.0 MB'), findsOneWidget);
      expect(find.textContaining('1 × 1'), findsOneWidget);
      // Read-only in S2: the switch is disabled.
      final switchWidget = tester.widget<Switch>(find.byType(Switch));
      expect(switchWidget.onChanged, isNull);
    });

    testWidgets('toggling in review mode updates the deletion plan',
        (tester) async {
      final image = makeImage();
      final container = ProviderContainer();
      addTearDown(container.dispose);
      await tester.pumpWidget(
        UncontrolledProviderScope(
          container: container,
          child: MaterialApp(
            home: Scaffold(
              body: DetailView(
                image: image,
                canToggleDeletion: true,
                step: DeletionStep.quality,
                qualityFlagged: const <String>{'b.jpg'},
              ),
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();

      // a.jpg is unflagged → not marked by default.
      var switchWidget = tester.widget<Switch>(find.byType(Switch));
      expect(switchWidget.value, isFalse);

      await tester.tap(find.byType(Switch));
      await tester.pump();
      expect(
        container.read(deletionPlanProvider).explicitDeleted,
        contains('a.jpg'),
      );

      // Toggling again keeps it (rule 1 → rule 2).
      await tester.tap(find.byType(Switch));
      await tester.pump();
      switchWidget = tester.widget<Switch>(find.byType(Switch));
      expect(switchWidget.value, isFalse);
      expect(
        container.read(deletionPlanProvider).explicitKept,
        contains('a.jpg'),
      );
    });

    testWidgets('flagged images default to marked', (tester) async {
      final image = makeImage();
      final container = ProviderContainer();
      addTearDown(container.dispose);
      await tester.pumpWidget(
        UncontrolledProviderScope(
          container: container,
          child: MaterialApp(
            home: Scaffold(
              body: DetailView(
                image: image,
                canToggleDeletion: true,
                step: DeletionStep.quality,
                qualityFlagged: const <String>{'a.jpg'},
              ),
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final switchWidget = tester.widget<Switch>(find.byType(Switch));
      expect(switchWidget.value, isTrue);
    });
  });
}
