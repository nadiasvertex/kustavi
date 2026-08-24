import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:kustavi/src/ui/widgets/image_grid.dart';

Widget _cell(BuildContext context, int index) =>
    Container(key: ValueKey('cell-$index'), color: const Color(0xFF888888));

/// The grid inside a bounded region, mirroring the quality review
/// ([Expanded] in a plain [Column]).
Widget _boundedGrid() => const MaterialApp(
  home: Scaffold(
    body: SizedBox(
      width: 400,
      height: 400,
      child: Column(
        children: [Expanded(child: ImageGrid(count: 12, builder: _cell))],
      ),
    ),
  ),
);

/// The grid in a [SliverList] item of a [CustomScrollView], mirroring the
/// similar / trips reviews.
Widget _unboundedGrid() => MaterialApp(
  home: Scaffold(
    body: CustomScrollView(
      slivers: [
        const SliverToBoxAdapter(child: SizedBox(height: 40)),
        SliverList(
          delegate: SliverChildListDelegate([
            const Padding(
              padding: EdgeInsets.fromLTRB(16, 8, 16, 8),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text('Group 1 · 12 photos'),
                  ImageGrid(count: 12, builder: _cell),
                ],
              ),
            ),
          ]),
        ),
      ],
    ),
  ),
);

void main() {
  group('ImageGrid (§6.2, §7.1)', () {
    testWidgets('scrolls on its own in a bounded region', (tester) async {
      await tester.pumpWidget(_boundedGrid());
      final scrollable = tester.state<ScrollableState>(
        find.descendant(
          of: find.byType(GridView),
          matching: find.byType(Scrollable),
        ),
      );
      expect(scrollable.position.maxScrollExtent, greaterThan(0));
      await tester.drag(find.byType(GridView), const Offset(0, -200));
      await tester.pump();
      expect(scrollable.position.pixels, greaterThan(0));
    });

    testWidgets('defers to the outer scrollable in a sliver context', (
      tester,
    ) async {
      await tester.pumpWidget(_unboundedGrid());
      final gridScrollable = tester.state<ScrollableState>(
        find.descendant(
          of: find.byType(GridView),
          matching: find.byType(Scrollable),
        ),
      );
      expect(gridScrollable.position.maxScrollExtent, 0);

      // Dragging over the grid scrolls the enclosing CustomScrollView.
      final lastCell = find.byKey(const ValueKey('cell-11'));
      expect(lastCell, findsOneWidget);
      final before = tester.getCenter(lastCell);
      await tester.drag(find.byType(CustomScrollView), const Offset(0, -300));
      await tester.pump();
      expect(tester.getCenter(lastCell).dy, lessThan(before.dy));
    });
  });
}
