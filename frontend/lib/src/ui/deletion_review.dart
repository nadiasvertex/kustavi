import 'package:flutter/material.dart' hide ImageInfo;
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../state/decisions.dart';
import '../state/domain.dart';
import '../state/wizard.dart';
import 'format.dart';
import 'widgets/badges.dart';
import 'widgets/image_cell.dart';
import 'widgets/image_grid.dart';

/// Final review: every photo marked for deletion, grouped by batch, with the
/// reason each pass gave. Tapping a photo keeps it.
class DeletionReviewScreen extends ConsumerWidget {
  const DeletionReviewScreen({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final theme = Theme.of(context);
    ref.watch(wizardProvider);
    ref.watch(deletionPlanProvider);
    final wizard = ref.read(wizardProvider.notifier);
    final groups = wizard.markedByBatch;
    final total = groups.fold<int>(0, (sum, g) => sum + g.images.length);

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 8, 16, 4),
          child: Text(
            total == 0
                ? 'Nothing is marked for deletion'
                : '${formatInt(total)} photos marked for deletion',
            style: theme.textTheme.titleLarge,
          ),
        ),
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
          child: Text(
            'Tap a photo to keep it. Nothing is deleted: kept photos are '
            'copied to the destination and the rest stay in the source folder.',
            style: theme.textTheme.bodyMedium?.copyWith(
              color: theme.colorScheme.onSurfaceVariant,
            ),
          ),
        ),
        const Divider(height: 1),
        Expanded(
          child: CustomScrollView(
            slivers: [
              for (final group in groups) ...[
                SliverToBoxAdapter(
                  child: Padding(
                    padding: const EdgeInsets.fromLTRB(16, 12, 16, 0),
                    child: Text(
                      '${group.title} · ${formatInt(group.images.length)}',
                      style: theme.textTheme.titleSmall?.copyWith(
                        fontWeight: FontWeight.w600,
                      ),
                    ),
                  ),
                ),
                SliverToBoxAdapter(
                  child: ImageGrid(
                    count: group.images.length,
                    builder: (context, index) {
                      final ImageInfo image = group.images[index];
                      return ImageCell(
                        key: ValueKey('del:${image.id}'),
                        image: image,
                        chips: [
                          for (final reason in wizard.deletionReasons(image.id))
                            ReasonChip(reason),
                        ],
                        onTap: () => ref
                            .read(deletionPlanProvider.notifier)
                            .keep(image.id),
                      );
                    },
                  ),
                ),
              ],
            ],
          ),
        ),
      ],
    );
  }
}
