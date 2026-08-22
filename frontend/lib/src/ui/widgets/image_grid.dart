import 'package:flutter/material.dart';

/// Lazy adaptive image grid targeting ~176px cells
/// (spec/frontend.md §7.1).
class ImageGrid extends StatelessWidget {
  const ImageGrid({super.key, required this.count, required this.builder});

  final int count;
  final Widget Function(BuildContext context, int index) builder;

  @override
  Widget build(BuildContext context) {
    return GridView.builder(
      padding: const EdgeInsets.fromLTRB(16, 8, 16, 16),
      gridDelegate: const SliverGridDelegateWithMaxCrossAxisExtent(
        maxCrossAxisExtent: 280,
        mainAxisSpacing: 12,
        crossAxisSpacing: 12,
        childAspectRatio: 1,
      ),
      itemCount: count,
      itemBuilder: builder,
    );
  }
}
