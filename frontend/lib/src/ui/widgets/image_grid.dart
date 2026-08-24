import 'package:flutter/material.dart';

/// A bounded image grid with fixed cell sizing that fits inside a [Column].
///
/// Use this when the grid is placed inside a [Column] inside a scrollable
/// (e.g. [SliverList] delegate) — it constrains itself to the available
/// space rather than scrolling independently like the original [GridView.builder].
class ImageGrid extends StatelessWidget {
  const ImageGrid({super.key, required this.count, required this.builder});

  final int count;
  final Widget Function(BuildContext context, int index) builder;

  @override
  Widget build(BuildContext context) {
    if (count == 0) {
      return const SizedBox.shrink();
    }

    // Determine grid dimensions so we use a fixed cross-axis count.
    // We pick the same maxCrossAxisExtent as the old GridView.builder (280).
    final mediaWidth = MediaQuery.sizeOf(context).width;
    final crossAxisSpacing = 12.0;
    final maxCrossAxisExtent = 280.0;
    final padding = 32.0;
    final usableWidth = mediaWidth - padding;
    final columnCount = ((usableWidth + crossAxisSpacing) /
        (maxCrossAxisExtent + crossAxisSpacing))
        .floor()
        .clamp(1, 6);

    return GridView.builder(
      padding: const EdgeInsets.fromLTRB(16, 8, 16, 16),
      gridDelegate: SliverGridDelegateWithFixedCrossAxisCount(
        crossAxisCount: columnCount,
        mainAxisSpacing: 12,
        crossAxisSpacing: 12,
        childAspectRatio: 1,
      ),
      itemCount: count,
      itemBuilder: builder,
      // Don't scroll — take up only as much space as the cells need.
      shrinkWrap: true,
      physics: const NeverScrollableScrollPhysics(),
    );
  }
}
