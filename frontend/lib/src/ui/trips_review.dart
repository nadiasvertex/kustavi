import 'package:flutter/material.dart' hide ImageInfo;
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../state/decisions.dart';
import '../state/domain.dart';
import '../state/wizard.dart';
import 'format.dart';
import 'widgets/image_cell.dart';
import 'widgets/image_grid.dart';

/// S10 — trips review: home-anchored spatiotemporal clusters grouped into
/// named folders, with per-photo trip curation (spec/frontend.md §6.2, §15).
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
    final unassigned = wizard.unassignedTripImageIds;

    return CustomScrollView(
      slivers: [
        SliverToBoxAdapter(
          child: Padding(
            padding: const EdgeInsets.fromLTRB(16, 8, 16, 0),
            child: Text(
              '$folderCount folders · $tripCount trips · $markedCount marked'
              '${unassigned.isEmpty ? '' : ' · ${unassigned.length} unassigned'}',
              style: theme.textTheme.titleLarge,
            ),
          ),
        ),
        SliverToBoxAdapter(child: _ClusteringControls(wizard: wizard)),
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
        if (unassigned.isNotEmpty)
          SliverToBoxAdapter(
            child: _UnassignedSection(imageIds: unassigned, wizard: wizard),
          ),
        const SliverToBoxAdapter(child: SizedBox(height: 24)),
      ],
    );
  }
}

/// Sliders for the four clustering knobs plus the commit-layout switch.
class _ClusteringControls extends StatefulWidget {
  const _ClusteringControls({required this.wizard});

  final Wizard wizard;

  @override
  State<_ClusteringControls> createState() => _ClusteringControlsState();
}

class _ClusteringControlsState extends State<_ClusteringControls> {
  late double _gap = widget.wizard.tripGapHours.toDouble();
  late double _distance = widget.wizard.tripDistanceKm.toDouble();
  late double _homeRadius = widget.wizard.tripHomeRadiusKm.toDouble();
  late double _legRadius = widget.wizard.tripLegRadiusKm.toDouble();

  void _recluster() {
    widget.wizard.rerunTripsPass(
      gapHours: _gap.round(),
      distanceKm: _distance.round(),
      homeRadiusKm: _homeRadius.round(),
      legRadiusKm: _legRadius.round(),
    );
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(
        content: Text('Re-clustering — manual trip edits were cleared'),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 4, 16, 4),
      child: ExpansionTile(
        title: const Text('Clustering settings'),
        childrenPadding: const EdgeInsets.fromLTRB(8, 0, 8, 8),
        children: [
          _slider('Max time gap', _gap, 1, 168, 'h',
              (v) => setState(() => _gap = v)),
          _slider('Max trip drift', _distance, 10, 1000, 'km',
              (v) => setState(() => _distance = v)),
          _slider('Away-from-home distance', _homeRadius, 1, 100, 'km',
              (v) => setState(() => _homeRadius = v)),
          _slider('New-leg distance', _legRadius, 1, 200, 'km',
              (v) => setState(() => _legRadius = v)),
          Row(
            children: [
              Expanded(
                child: SwitchListTile(
                  contentPadding: EdgeInsets.zero,
                  dense: true,
                  title: const Text('Organize output into trip folders'),
                  value: widget.wizard.organizeIntoTripFolders,
                  onChanged: (v) =>
                      setState(() => widget.wizard.organizeIntoTripFolders = v),
                ),
              ),
              const SizedBox(width: 12),
              FilledButton.icon(
                onPressed: _recluster,
                icon: const Icon(Icons.refresh),
                label: const Text('Re-cluster'),
              ),
            ],
          ),
        ],
      ),
    );
  }

  Widget _slider(String label, double value, double min, double max,
      String unit, ValueChanged<double> onChanged) {
    return Row(
      children: [
        SizedBox(
          width: 190,
          child: Text('$label: ${value.round()} $unit'),
        ),
        Expanded(
          child: Slider(
            value: value.clamp(min, max),
            min: min,
            max: max,
            onChanged: onChanged,
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
  late final TextEditingController _controller;
  late final FocusNode _focusNode;
  bool _editing = false;

  @override
  void initState() {
    super.initState();
    _controller = TextEditingController(text: widget.folder.name);
    _focusNode = FocusNode();
  }

  @override
  void didUpdateWidget(_FolderSection oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.folder.name != widget.folder.name && !_editing) {
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
      for (final trip in widget.folder.trips) {
        widget.wizard.renameTripFolder(trip.id, newName);
      }
    }
    setState(() => _editing = false);
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
          Container(
            width: double.infinity,
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
            decoration: BoxDecoration(
              color: theme.colorScheme.surfaceContainerHighest,
              borderRadius: BorderRadius.circular(8),
            ),
            child: Row(
              children: [
                Icon(Icons.folder, size: 18, color: theme.colorScheme.primary),
                const SizedBox(width: 8),
                Expanded(
                  child: _editing
                      ? TextField(
                          controller: _controller,
                          focusNode: _focusNode,
                          decoration: const InputDecoration(
                            contentPadding: EdgeInsets.zero,
                            isDense: true,
                            border: InputBorder.none,
                          ),
                          style: theme.textTheme.titleSmall?.copyWith(
                            fontWeight: FontWeight.w600,
                            color: theme.colorScheme.onSurfaceVariant,
                          ),
                          onTapOutside: (_) => _finishEditing(),
                          onSubmitted: (_) => _finishEditing(),
                          onEditingComplete: _finishEditing,
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
                  '$tripCount ${tripCount == 1 ? 'trip' : 'trips'}',
                  style: theme.textTheme.labelSmall?.copyWith(
                    color: theme.colorScheme.onSurfaceVariant,
                  ),
                ),
                const SizedBox(width: 4),
                Icon(_editing ? Icons.check : Icons.edit,
                    size: 16, color: theme.colorScheme.onSurfaceVariant),
              ],
            ),
          ),
          const SizedBox(height: 4),
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

class _TripWidget extends ConsumerStatefulWidget {
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
  ConsumerState<_TripWidget> createState() => _TripWidgetState();
}

class _TripWidgetState extends ConsumerState<_TripWidget> {
  bool _selecting = false;
  final Set<String> _selected = <String>{};

  void _exitSelecting() {
    setState(() {
      _selecting = false;
      _selected.clear();
    });
  }

  void _toggleDeletionMark(String imageId) {
    final selections = widget.wizard.tripSelections
        .putIfAbsent(widget.trip.id, () => <String>{});
    if (!selections.remove(imageId)) {
      selections.add(imageId);
    }
    ref.read(wizardProvider);
    setState(() {});
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final trip = widget.trip;
    final selections =
        widget.wizard.tripSelections[trip.id] ?? const <String>{};

    final label = trip.isHome
        ? 'Home'
        : (trip.placeName.isNotEmpty ? trip.placeName : (trip.folder ?? 'Trip'));

    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 4, 16, 8),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Icon(
                trip.isHome ? Icons.home : Icons.flight_takeoff,
                size: 16,
                color: theme.colorScheme.primary,
              ),
              const SizedBox(width: 8),
              Expanded(
                child: Text(
                  '$label  ·  '
                  '${trip.start.toIso8601String().substring(0, 10)}'
                  ' – ${trip.end.toIso8601String().substring(0, 10)}',
                  style: theme.textTheme.bodyMedium
                      ?.copyWith(fontWeight: FontWeight.w500),
                ),
              ),
              Text(
                '${trip.memberIds.length} photos · '
                '${formatDuration(trip.start, trip.end)}',
                style: theme.textTheme.labelSmall
                    ?.copyWith(color: theme.colorScheme.onSurfaceVariant),
              ),
              IconButton(
                tooltip: _selecting ? 'Cancel selection' : 'Select photos',
                visualDensity: VisualDensity.compact,
                icon: Icon(_selecting ? Icons.close : Icons.checklist),
                onPressed: () => _selecting ? _exitSelecting() : setState(
                    () => _selecting = true),
              ),
            ],
          ),
          if (trip.legs.length <= 1)
            _grid(trip.memberIds)
          else
            ...trip.legs.map((leg) => Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Padding(
                      padding: const EdgeInsets.fromLTRB(16, 4, 16, 0),
                      child: Text(
                        leg.placeName.isNotEmpty
                            ? leg.placeName
                            : 'Leg (${leg.memberIds.length})',
                        style: theme.textTheme.labelMedium?.copyWith(
                          color: theme.colorScheme.onSurfaceVariant,
                        ),
                      ),
                    ),
                    _grid(leg.memberIds),
                  ],
                )),
          if (_selecting && _selected.isNotEmpty)
            _MoveBar(
              count: _selected.length,
              targets: _moveTargets(),
              onMove: (tripId) {
                widget.wizard.moveImagesToTrip(Set.of(_selected), tripId);
                _exitSelecting();
              },
              onNewTrip: () {
                widget.wizard.createTripFromImages(Set.of(_selected));
                _exitSelecting();
              },
            )
          else if (!_selecting && selections.isNotEmpty)
            Padding(
              padding: const EdgeInsets.only(left: 16, top: 4),
              child: Text(
                '${selections.length} selected for deletion',
                style: theme.textTheme.bodySmall
                    ?.copyWith(color: theme.colorScheme.error),
              ),
            ),
        ],
      ),
    );
  }

  List<_MoveTarget> _moveTargets() {
    final targets = <_MoveTarget>[
      const _MoveTarget(null, 'Remove from trips'),
    ];
    for (final other in widget.wizard.tripResults) {
      if (other.id == widget.trip.id) continue;
      final name = other.isHome
          ? 'Home'
          : (other.placeName.isNotEmpty
              ? other.placeName
              : (other.folder ?? 'Trip'));
      targets.add(_MoveTarget(
        other.id,
        '$name · ${other.start.toIso8601String().substring(0, 10)}',
      ));
    }
    return targets;
  }

  Widget _grid(List<String> ids) {
    return ImageGrid(
      count: ids.length,
      builder: (context, index) {
        final imageId = ids[index];
        if (widget.plan.explicitDeleted.contains(imageId)) {
          return const SizedBox.shrink();
        }
        final image = widget.wizard.images[imageId];
        if (image == null) return const SizedBox.shrink();

        final selections =
            widget.wizard.tripSelections[widget.trip.id] ?? const <String>{};

        if (_selecting) {
          final picked = _selected.contains(imageId);
          return Stack(
            fit: StackFit.expand,
            children: [
              ImageCell(image: image, marked: selections.contains(imageId)),
              Positioned.fill(
                child: GestureDetector(
                  onTap: () => setState(() {
                    if (!_selected.remove(imageId)) _selected.add(imageId);
                  }),
                  child: DecoratedBox(
                    decoration: BoxDecoration(
                      borderRadius: BorderRadius.circular(8),
                      border: Border.all(
                        color: picked
                            ? Theme.of(context).colorScheme.primary
                            : Colors.transparent,
                        width: 3,
                      ),
                      color: picked
                          ? Theme.of(context)
                              .colorScheme
                              .primary
                              .withValues(alpha: 0.18)
                          : Colors.transparent,
                    ),
                    child: picked
                        ? Align(
                            alignment: Alignment.topLeft,
                            child: Padding(
                              padding: const EdgeInsets.all(6),
                              child: Icon(
                                Icons.check_circle,
                                color:
                                    Theme.of(context).colorScheme.primary,
                              ),
                            ),
                          )
                        : const SizedBox.shrink(),
                  ),
                ),
              ),
            ],
          );
        }

        return ImageCell(
          image: image,
          marked: selections.contains(imageId),
          onTap: () => _toggleDeletionMark(imageId),
        );
      },
    );
  }
}

/// The "Unassigned / no location" section: photos in no trip.
class _UnassignedSection extends ConsumerStatefulWidget {
  const _UnassignedSection({required this.imageIds, required this.wizard});

  final List<String> imageIds;
  final Wizard wizard;

  @override
  ConsumerState<_UnassignedSection> createState() => _UnassignedSectionState();
}

class _UnassignedSectionState extends ConsumerState<_UnassignedSection> {
  final Set<String> _selected = <String>{};

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 8, 16, 4),
      child: ExpansionTile(
        title: Text('Unassigned · ${widget.imageIds.length} photos'),
        subtitle: const Text('Not in any trip (no GPS/timestamp, or removed)'),
        childrenPadding: const EdgeInsets.only(bottom: 8),
        children: [
          if (_selected.isNotEmpty)
            _MoveBar(
              count: _selected.length,
              addVerb: 'Add',
              targets: [
                for (final trip in widget.wizard.tripResults)
                  _MoveTarget(
                    trip.id,
                    '${trip.isHome ? 'Home' : (trip.placeName.isNotEmpty ? trip.placeName : (trip.folder ?? 'Trip'))}'
                    ' · ${trip.start.toIso8601String().substring(0, 10)}',
                  ),
              ],
              onMove: (tripId) {
                widget.wizard.moveImagesToTrip(Set.of(_selected), tripId);
                setState(_selected.clear);
              },
              onNewTrip: () {
                widget.wizard.createTripFromImages(Set.of(_selected));
                setState(_selected.clear);
              },
            ),
          ImageGrid(
            count: widget.imageIds.length,
            builder: (context, index) {
              final id = widget.imageIds[index];
              final image = widget.wizard.images[id];
              if (image == null) return const SizedBox.shrink();
              final picked = _selected.contains(id);
              return GestureDetector(
                onTap: () => setState(() {
                  if (!_selected.remove(id)) _selected.add(id);
                }),
                child: DecoratedBox(
                  decoration: BoxDecoration(
                    borderRadius: BorderRadius.circular(8),
                    border: Border.all(
                      color: picked
                          ? theme.colorScheme.primary
                          : Colors.transparent,
                      width: 3,
                    ),
                  ),
                  child: ImageCell(image: image, marked: false),
                ),
              );
            },
          ),
        ],
      ),
    );
  }
}

class _MoveTarget {
  const _MoveTarget(this.tripId, this.label);
  final int? tripId;
  final String label;
}

class _MoveBar extends StatelessWidget {
  const _MoveBar({
    required this.count,
    required this.targets,
    required this.onMove,
    required this.onNewTrip,
    this.addVerb = 'Move',
  });

  final int count;
  final List<_MoveTarget> targets;
  final ValueChanged<int?> onMove;
  final VoidCallback onNewTrip;
  final String addVerb;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 4, 16, 4),
      child: Row(
        children: [
          Text('$count selected'),
          const SizedBox(width: 12),
          PopupMenuButton<int>(
            child: Chip(
              label: Text('$addVerb to trip ▸'),
              avatar: const Icon(Icons.drive_file_move, size: 18),
            ),
            itemBuilder: (context) => [
              const PopupMenuItem(value: -2, child: Text('New trip')),
              const PopupMenuDivider(),
              for (var i = 0; i < targets.length; i++)
                PopupMenuItem(value: i, child: Text(targets[i].label)),
            ],
            onSelected: (value) {
              if (value == -2) {
                onNewTrip();
              } else {
                onMove(targets[value].tripId);
              }
            },
          ),
        ],
      ),
    );
  }
}
