import 'package:flutter/material.dart' hide ImageInfo;
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../state/domain.dart';
import '../state/decisions.dart';
import '../state/wizard.dart';
import 'format.dart';
import 'widgets/image_cell.dart';
import 'widgets/image_grid.dart';

/// S10 — trips review: spatiotemporal clusters grouped into named folders
/// (spec/frontend.md §6.2, §15).
class TripsReviewScreen extends ConsumerWidget {
  const TripsReviewScreen({
    super.key,
    required this.tripCount,
    required this.markedCount,
    required this.trips,
    required this.tripFolders,
  });

  final int tripCount;
  final int markedCount;
  final List<TripInfo> trips;
  final List<TripFolderInfo> tripFolders;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final theme = Theme.of(context);
    ref.watch(wizardProvider);
    final wizard = ref.read(wizardProvider.notifier);
    final plan = ref.watch(deletionPlanProvider);

    final folderCount = tripFolders.length;

    return CustomScrollView(
      slivers: [
        SliverToBoxAdapter(
          child: Padding(
            padding: const EdgeInsets.fromLTRB(16, 8, 16, 0),
            child: Text(
              '$folderCount folders · $tripCount trips · $markedCount marked',
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
                final folder = tripFolders[index];
                return _FolderSection(
                  key: ValueKey(folder.name),
                  folder: folder,
                  plan: plan,
                  wizard: wizard,
                );
              },
              childCount: tripFolders.length,
              addAutomaticKeepAlives: false,
              addRepaintBoundaries: false,
            ),
          ),
      ],
    );
  }
}

class _FolderSection extends ConsumerStatefulWidget {
  const _FolderSection({
    super.key,
    required this.folder,
    required this.plan,
    required this.wizard,
  });

  final TripFolderInfo folder;
  final DeletionIntent plan;
  final Wizard wizard;

  @override
  ConsumerState<_FolderSection> createState() => _FolderSectionState();
}

class _FolderSectionState extends ConsumerState<_FolderSection> {
  late final String _initialName;
  late final TextEditingController _controller;
  late final FocusNode _focusNode;
  bool _editing = false;

  @override
  void initState() {
    super.initState();
    _initialName = widget.folder.name;
    _controller = TextEditingController(text: _initialName);
    _focusNode = FocusNode();
  }

  @override
  void didUpdateWidget(_FolderSection oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.folder.name != widget.folder.name) {
      _controller.text = widget.folder.name;
    }
  }

  @override
  void dispose() {
    _controller.dispose();
    _focusNode.dispose();
    super.dispose();
  }

  void _startEditing() {
    setState(() {
      _editing = true;
      _controller.text = widget.folder.name;
    });
    FocusScope.of(context).requestFocus(_focusNode);
  }

  void _finishEditing() {
    final newName = _controller.text.trim();
    if (newName.isNotEmpty && newName != widget.folder.name) {
      // Apply the name change to all trips in this folder.
      for (final trip in widget.folder.trips) {
        widget.wizard.renameTripFolder(trip.id, newName);
      }
    }
    setState(() {
      _editing = false;
    });
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final tripCount = widget.folder.trips.length;

    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 4, 16, 4),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          // Folder header
          Container(
            width: double.infinity,
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
            decoration: BoxDecoration(
              color: theme.colorScheme.surfaceContainerHighest,
              borderRadius: BorderRadius.circular(8),
            ),
            child: Row(
              children: [
                Icon(
                  Icons.folder,
                  size: 18,
                  color: theme.colorScheme.primary,
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: _editing
                      ? TextField(
                          controller: _controller,
                          focusNode: _focusNode,
                          decoration: InputDecoration(
                            contentPadding: EdgeInsets.zero,
                            isDense: true,
                            border: InputBorder.none,
                            errorBorder: UnderlineInputBorder(
                              borderSide:
                                  BorderSide(color: theme.colorScheme.error),
                            ),
                          ),
                          style: theme.textTheme.titleSmall?.copyWith(
                            fontWeight: FontWeight.w600,
                            color: theme.colorScheme.onSurfaceVariant,
                          ),
                          onTapOutside: (_) => _finishEditing(),
                          onSubmitted: (_) => _finishEditing(),
                          onEditingComplete: _finishEditing,
                          onTap: () {
                            // Allow re-editing while already focused.
                          },
                        )
                      : GestureDetector(
                          onTap: _startEditing,
                          child: Text(
                            widget.folder.name,
                            style: theme.textTheme.titleSmall?.copyWith(
                              fontWeight: FontWeight.w600,
                              color: theme.colorScheme.onSurfaceVariant,
                            ),
                          ),
                        ),
                ),
                const SizedBox(width: 8),
                Text(
                  '$tripCount trips',
                  style: theme.textTheme.labelSmall?.copyWith(
                    color: theme.colorScheme.onSurfaceVariant,
                  ),
                ),
                const SizedBox(width: 4),
                Icon(
                  _editing ? Icons.check : Icons.edit,
                  size: 16,
                  color: theme.colorScheme.onSurfaceVariant,
                ),
              ],
            ),
          ),
          const SizedBox(height: 4),

          // Trips within the folder
          ...widget.folder.trips.map(
            (trip) => _TripWidget(
              key: ValueKey(trip.id),
              trip: trip,
              plan: widget.plan,
              wizard: widget.wizard,
            ),
          ),
          const Divider(height: 1),
        ],
      ),
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
      padding: const EdgeInsets.fromLTRB(16, 4, 16, 4),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          // Trip header with date range and member count
          Row(
            children: [
              Icon(
                Icons.flight_takeoff,
                size: 16,
                color: theme.colorScheme.primary,
              ),
              const SizedBox(width: 8),
              Text(
                trip.start.toIso8601String().substring(0, 10),
                style: theme.textTheme.bodyMedium?.copyWith(
                  fontWeight: FontWeight.w500,
                ),
              ),
              Text(
                ' — ${trip.end.toIso8601String().substring(0, 10)}',
                style: theme.textTheme.bodyMedium?.copyWith(
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
          const SizedBox(height: 4),

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
