import 'package:flutter/material.dart' hide ImageInfo;
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../state/domain.dart';
import '../state/decisions.dart';
import '../state/wizard.dart';
import 'format.dart';
import 'widgets/image_cell.dart';
import 'widgets/image_grid.dart';

/// S10 — trips review: spatiotemporal clusters with deletion toggles
/// (spec/frontend.md §6.2).
class TripsReviewScreen extends ConsumerWidget {
  const TripsReviewScreen({
    super.key,
    required this.tripCount,
    required this.markedCount,
    required this.trips,
  });

  final int tripCount;
  final int markedCount;
  final List<TripInfo> trips;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final theme = Theme.of(context);
    ref.watch(wizardProvider);
    final wizard = ref.read(wizardProvider.notifier);
    final plan = ref.watch(deletionPlanProvider);

    return CustomScrollView(
      slivers: [
        SliverToBoxAdapter(
          child: Padding(
            padding: const EdgeInsets.fromLTRB(16, 8, 16, 0),
            child: Text(
              '$tripCount trips · $markedCount marked',
              style: theme.textTheme.titleLarge,
            ),
          ),
        ),
        if (trips.isEmpty)
          const SliverToBoxAdapter(
            child: Center(
              child: Padding(
                padding: EdgeInsets.all(32),
                child: Text(
                  'No trips found — photos may lack GPS or timestamp data',
                  style: TextStyle(fontSize: 16),
                ),
              ),
            ),
          )
        else
          SliverList(
            delegate: SliverChildBuilderDelegate(
              (context, index) {
                final trip = trips[index];
                return _TripWidget(
                  key: ValueKey(trip.id),
                  trip: trip,
                  plan: plan,
                  wizard: wizard,
                );
              },
              childCount: trips.length,
              addAutomaticKeepAlives: false,
              addRepaintBoundaries: false,
            ),
          ),
      ],
    );
  }
}

class _TripWidget extends ConsumerWidget {
  const _TripWidget({
    super.key,
    required this.trip,
    required this.plan,
    required this.wizard,
  });

  final TripInfo trip;
  final DeletionIntent plan;
  final Wizard wizard;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final theme = Theme.of(context);

    final selections = wizard.tripSelections[trip.id] ?? const <String>{};
    final hasSelections = selections.isNotEmpty;

    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 8, 16, 8),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          // Trip header with date range and member count
          Row(
            children: [
              Icon(
                Icons.flight_takeoff,
                size: 18,
                color: theme.colorScheme.primary,
              ),
              const SizedBox(width: 8),
              Text(
                trip.start.toIso8601String().substring(0, 10),
                style: theme.textTheme.titleSmall?.copyWith(
                  fontWeight: FontWeight.w600,
                ),
              ),
              Text(
                ' — ${trip.end.toIso8601String().substring(0, 10)}',
                style: theme.textTheme.titleSmall?.copyWith(
                  color: theme.colorScheme.onSurfaceVariant,
                ),
              ),
              const Spacer(),
              Text(
                '${trip.memberIds.length} photos · '
                '${formatDuration(trip.start, trip.end)}',
                style: theme.textTheme.labelSmall?.copyWith(
                  color: theme.colorScheme.onSurfaceVariant,
                ),
              ),
            ],
          ),
          if (trip.centroid != null) ...[
            const SizedBox(height: 2),
            Text(
              '${trip.centroid!.$1.toStringAsFixed(4)}, '
              '${trip.centroid!.$2.toStringAsFixed(4)}',
              style: theme.textTheme.bodySmall?.copyWith(
                color: theme.colorScheme.onSurfaceVariant,
              ),
            ),
          ],
          const SizedBox(height: 8),

          // Trip images in chronological order
          ImageGrid(
            count: trip.memberIds.length,
            builder: (context, index) {
              final imageId = trip.memberIds[index];
              final image = wizard.images[imageId];
              if (image == null) {
                return const SizedBox.shrink();
              }

              final isMarked = _isMarked(plan, imageId, selections);

              return ImageCell(
                image: image,
                marked: isMarked,
                onTap: () => _toggleSelection(context, ref, imageId),
              );
            },
          ),
          if (hasSelections) ...[
            Padding(
              padding: const EdgeInsets.only(left: 16, right: 16, bottom: 4),
              child: Text(
                '$selections selected for deletion',
                style: theme.textTheme.bodySmall?.copyWith(
                  color: theme.colorScheme.error,
                ),
              ),
            ),
          ],
          const Divider(height: 1),
        ],
      ),
    );
  }

  bool _isMarked(DeletionIntent plan, String id, Set<String> selections) {
    if (plan.explicitDeleted.contains(id)) return true;
    if (plan.explicitKept.contains(id)) return false;
    return selections.contains(id);
  }

  void _toggleSelection(
    BuildContext context,
    WidgetRef ref,
    String imageId,
  ) {
    final wizard = ref.read(wizardProvider.notifier);
    final tripId = trip.id;

    // The wizard._tripSelections is a mutable map, and each value is a mutable
    // set. We can modify the set in-place without triggering Riverpod.
    final selections = wizard.tripSelections.putIfAbsent(
      tripId,
      () => Set<String>.identity(),
    );
    if (selections.contains(imageId)) {
      selections.remove(imageId);
    } else {
      selections.add(imageId);
    }

    // Trigger a rebuild by touching the wizard state.
    ref.read(wizardProvider);
  }
}
