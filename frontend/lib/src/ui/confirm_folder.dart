import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../state/wizard.dart';
import 'format.dart';
import 'widgets/detail_view.dart';
import 'widgets/image_cell.dart';
import 'widgets/image_grid.dart';

/// S2 — confirm folder: every scanned image, read-only
/// (spec/frontend.md §6.2).
class ConfirmFolderScreen extends ConsumerWidget {
  const ConfirmFolderScreen({
    super.key,
    required this.folder,
    required this.imageCount,
    required this.scanErrors,
  });

  final String folder;
  final int imageCount;
  final List<String> scanErrors;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final theme = Theme.of(context);
    ref.watch(wizardProvider);
    final images = ref.read(wizardProvider.notifier).orderedImages;
    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 8, 16, 0),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(
            '${formatInt(imageCount)} images in $folder',
            style: theme.textTheme.titleLarge,
          ),
          if (scanErrors.isNotEmpty)
            Padding(
              padding: const EdgeInsets.only(top: 4),
              child: Text(
                '${scanErrors.length} '
                '${scanErrors.length == 1 ? 'file' : 'files'} '
                'could not be read',
                style: theme.textTheme.bodySmall
                    ?.copyWith(color: theme.colorScheme.error),
              ),
            ),
          const SizedBox(height: 12),
          Expanded(
            child: ImageGrid(
              count: images.length,
              builder: (context, index) {
                final image = images[index];
                return ImageCell(
                  image: image,
                  onTap: () => showImageDetail(
                    context,
                    image: image,
                    canToggleDeletion: false,
                  ),
                );
              },
            ),
          ),
        ],
      ),
    );
  }
}
