import 'package:flutter/material.dart' hide ImageInfo;
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../state/domain.dart';
import '../state/decisions.dart';
import '../state/wizard.dart';
import 'widgets/detail_view.dart';
import 'widgets/image_cell.dart';
import 'widgets/image_grid.dart';

/// S9 — similar-image review: groups with keeper selection and deletion toggles
/// (spec/frontend.md §6.2).
class SimilarReviewScreen extends ConsumerWidget {
  const SimilarReviewScreen({
    super.key,
    required this.groupCount,
    required this.markedCount,
  });

  final int groupCount;
  final int markedCount;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final theme = Theme.of(context);
    ref.watch(wizardProvider);
    final wizard = ref.read(wizardProvider.notifier);
    final plan = ref.watch(deletionPlanProvider);
    final groups = wizard.similarGroups;
    final keepers = similarKeeperMap(plan, groups);
    final totalImages = groups.expand((g) => g.memberIds).length;

    return CustomScrollView(
      slivers: [
        SliverToBoxAdapter(
          child: Padding(
            padding: const EdgeInsets.fromLTRB(16, 8, 16, 0),
            child: Text(
              '$groupCount groups · $totalImages images',
              style: theme.textTheme.titleLarge,
            ),
          ),
        ),
        if (groups.isEmpty)
          const SliverToBoxAdapter(
            child: Center(
              child: Padding(
                padding: EdgeInsets.all(32),
                child: Text(
                  'No similar photos found',
                  style: TextStyle(fontSize: 16),
                ),
              ),
            ),
          )
        else
          SliverList(
            delegate: SliverChildBuilderDelegate(
              (context, index) {
                final group = groups[index];
                return _GroupWidget(
                  key: ValueKey(group.id),
                  group: group,
                  plan: plan,
                  wizard: wizard,
                  keepers: keepers,
                );
              },
              childCount: groups.length,
              addAutomaticKeepAlives: false,
              addRepaintBoundaries: false,
            ),
          ),
      ],
    );
  }
}

class _GroupWidget extends StatelessWidget {
  const _GroupWidget({
    super.key,
    required this.group,
    required this.plan,
    required this.wizard,
    required this.keepers,
  });

  final SimilarGroupInfo group;
  final DeletionIntent plan;
  final Wizard wizard;
  final Map<String, String> keepers;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final keeper = plan.userKeeperOf(group.id) ?? group.recommendedKeepId;

    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 8, 16, 8),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(
            'Group ${group.id + 1} · ${group.memberIds.length} photos',
            style: theme.textTheme.labelLarge?.copyWith(
              color: theme.colorScheme.onSurfaceVariant,
            ),
          ),
          const SizedBox(height: 8),
          ImageGrid(
            count: group.memberIds.length,
            builder: (context, index) {
              final imageId = group.memberIds[index];
              final image = wizard.images[imageId];
              if (image == null) {
                return const SizedBox.shrink();
              }

              final isKeeper = imageId == keeper;
              final isSuggestedKeeper =
                  plan.userKeeperOf(group.id) == null && isKeeper;
              final isMarked = _isMarkedForDeletion(
                plan,
                imageId,
                keeper,
              );

              return ImageCell(
                image: image,
                marked: isMarked,
                keeper: isKeeper,
                suggestedKeeper: isSuggestedKeeper,
                onTap: () => _openDetail(context),
              );
            },
          ),
          const Divider(height: 1),
        ],
      ),
    );
  }

  bool _isMarkedForDeletion(
    DeletionIntent plan,
    String id,
    String keeper,
  ) {
    if (plan.explicitDeleted.contains(id)) return true;
    if (plan.explicitKept.contains(id)) return false;
    return id != keeper;
  }

  void _openDetail(BuildContext context) {
    // Open detail for the first non-keeper (we need an image from the group)
    final firstImageId = group.memberIds.first;
    final image = wizard.images[firstImageId];
    if (image == null) return;

    showImageDetail(
      context,
      image: image,
      canToggleDeletion: true,
      step: DeletionStep.similar,
      qualityFlagged: wizard.qualityFlags.keys.toSet(),
      junkFlagged: wizard.junkFlags.keys.toSet(),
      similarKeepers: keepers,
    );
  }
}
