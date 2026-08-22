import 'dart:io';

import 'package:flutter/material.dart' hide ImageInfo;

import '../../state/domain.dart';
import 'badges.dart';

/// A grid cell: the cached 768px working preview, an ellipsized file name,
/// and live badges (spec/frontend.md §7.1).
class ImageCell extends StatelessWidget {
  const ImageCell({
    super.key,
    required this.image,
    this.chips = const <Widget>[],
    this.marked = false,
    this.keeper = false,
    this.suggestedKeeper = false,
    this.onTap,
  });

  final ImageInfo image;
  final List<Widget> chips;

  /// Shows the red delete tag (reflects the live deletion hierarchy).
  final bool marked;

  /// Shows the amber keeper badge.
  final bool keeper;
  final bool suggestedKeeper;
  final VoidCallback? onTap;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return GestureDetector(
      onTap: onTap,
      child: MouseRegion(
        cursor: onTap == null ? MouseCursor.defer : SystemMouseCursors.click,
        child: Stack(
          fit: StackFit.expand,
          children: [
            ClipRRect(
              borderRadius: BorderRadius.circular(8),
              child: Image(
                image: FileImage(File(image.workingImagePath)),
                fit: BoxFit.cover,
                errorBuilder: (context, error, stackTrace) => ColoredBox(
                  color: theme.colorScheme.surfaceContainerHighest,
                  child: const Center(child: Icon(Icons.broken_image)),
                ),
              ),
            ),
            Positioned(
              left: 8,
              right: 8,
              bottom: 8,
              child: Container(
                padding: const EdgeInsets.symmetric(
                  horizontal: 8,
                  vertical: 4,
                ),
                decoration: BoxDecoration(
                  color: Colors.black54,
                  borderRadius: BorderRadius.circular(6),
                ),
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Text(
                      image.name,
                      style: const TextStyle(color: Colors.white, fontSize: 11),
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                    ),
                    if (chips.isNotEmpty) ...[
                      const SizedBox(height: 4),
                      Wrap(spacing: 4, runSpacing: 4, children: chips),
                    ],
                  ],
                ),
              ),
            ),
            if (marked) const Positioned(top: 8, right: 8, child: DeleteTag()),
            if (keeper)
              Positioned(
                top: 8,
                left: 8,
                child: KeeperBadge(suggested: suggestedKeeper),
              ),
          ],
        ),
      ),
    );
  }
}
