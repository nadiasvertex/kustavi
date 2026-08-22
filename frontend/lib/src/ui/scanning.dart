import 'package:flutter/material.dart';

import 'format.dart';

/// S1 — folder scan in flight (spec/frontend.md §6.2).
class ScanningScreen extends StatelessWidget {
  const ScanningScreen({
    super.key,
    required this.folder,
    required this.imagesFound,
    required this.currentPath,
  });

  final String folder;
  final int imagesFound;
  final String currentPath;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Center(
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 560),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Text(
              'Scanning $folder',
              style: theme.textTheme.titleLarge,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
            ),
            const SizedBox(height: 20),
            const LinearProgressIndicator(),
            const SizedBox(height: 12),
            Text(
              'Found ${formatInt(imagesFound)} '
              '${imagesFound == 1 ? 'image' : 'images'}',
              style: theme.textTheme.bodyMedium,
            ),
            if (currentPath.isNotEmpty) ...[
              const SizedBox(height: 8),
              Text(
                currentPath,
                style: theme.textTheme.bodySmall
                    ?.copyWith(fontFamily: 'monospace'),
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
              ),
            ],
          ],
        ),
      ),
    );
  }
}
