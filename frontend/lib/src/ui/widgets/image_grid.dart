import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../state/wizard.dart';

/// An image grid with fixed cell sizing (~280px cells, 1–6 columns).
///
/// Adapts to its height constraints: with a bounded height (e.g. inside an
/// [Expanded] of a plain [Column]) it fills the space and scrolls on its
/// own; with an unbounded height (inside an already-scrollable ancestor,
/// e.g. a [SliverList] delegate) it shrinks to its content and lets that
/// ancestor scroll.
class ImageGrid extends ConsumerStatefulWidget {
  const ImageGrid({super.key, required this.count, required this.builder});

  final int count;
  final Widget Function(BuildContext context, int index) builder;

  @override
  ConsumerState<ImageGrid> createState() => _ImageGridState();
}

class _ImageGridState extends ConsumerState<ImageGrid> {
  int _version = 0;

  @override
  void didUpdateWidget(covariant ImageGrid oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.count != widget.count) {
      _version++;
    }
  }

  @override
  Widget build(BuildContext context) {
    // Watch wizard so this widget rebuilds whenever wizard state changes.
    // The version bump forces GridView.builder to re-enter the builder.
    // Guard against tests without ProviderScope.
    try {
      ref.watch(wizardProvider);
    } catch (_) {
      // No ProviderScope — test is isolated, ignore.
    }

    if (widget.count == 0) {
      return const SizedBox.shrink();
    }

    // Determine grid dimensions so we use a fixed cross-axis count.
    // We pick the same maxCrossAxisExtent as the old GridView.builder (280).
    final mediaWidth = MediaQuery.sizeOf(context).width;
    const crossAxisSpacing = 12.0;
    const maxCrossAxisExtent = 280.0;
    const padding = 32.0;
    final usableWidth = mediaWidth - padding;
    final columnCount =
        ((usableWidth + crossAxisSpacing) /
                (maxCrossAxisExtent + crossAxisSpacing))
            .floor()
            .clamp(1, 6);

    return LayoutBuilder(
      builder: (context, constraints) {
        final boundedHeight = constraints.maxHeight.isFinite;
        return GridView.builder(
          key: ValueKey('ig_${widget.count}_v$_version'),
          padding: const EdgeInsets.fromLTRB(16, 8, 16, 16),
          gridDelegate: SliverGridDelegateWithFixedCrossAxisCount(
            crossAxisCount: columnCount,
            mainAxisSpacing: 12,
            crossAxisSpacing: 12,
            childAspectRatio: 1,
          ),
          itemCount: widget.count,
          itemBuilder: widget.builder,
          // A bounded region scrolls the grid itself; an unbounded (sliver)
          // context must not, so the grid fits its cells and defers to the
          // outer scrollable.
          shrinkWrap: !boundedHeight,
          physics: boundedHeight ? null : const NeverScrollableScrollPhysics(),
        );
      },
    );
  }
}
