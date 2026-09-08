import 'package:flutter/material.dart' hide ImageInfo;
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../state/decisions.dart';
import '../state/domain.dart';
import '../state/wizard.dart';
import 'widgets/badges.dart';
import 'widgets/detail_view.dart';
import 'widgets/image_cell.dart';
import 'widgets/image_grid.dart';

/// Flagged-candidate review shared by S4 (quality), S7 (junk) and S10-C
/// (video) (spec/frontend.md §6.2).
///
/// The screen is split into two sections: photos the user wants to **keep**
/// fill the main area, and everything still **marked for deletion** sits in a
/// collapsible panel at the bottom. Tapping a cell moves it between the two —
/// no need to open the detail view first. Every flagged image starts in the
/// delete panel (the pass flagged it), so the flow is "rescue the keepers".
class FlaggedReview extends ConsumerStatefulWidget {
  const FlaggedReview({super.key, required this.step, required this.header});

  final DeletionStep step;
  final String header;

  @override
  ConsumerState<FlaggedReview> createState() => _FlaggedReviewState();
}

class _FlaggedReviewState extends ConsumerState<FlaggedReview> {
  bool _deletePanelExpanded = true;

  DeletionStep get step => widget.step;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    ref.watch(wizardProvider);
    final wizard = ref.read(wizardProvider.notifier);
    final plan = ref.watch(deletionPlanProvider);
    final qualityFlagged = wizard.qualityFlags.keys.toSet();
    final junkFlagged = wizard.junkFlags.keys.toSet();
    final videoFlagged = wizard.videoFlags.keys.toSet();
    final flaggedIds = switch (step) {
      DeletionStep.quality => qualityFlagged,
      DeletionStep.junk => junkFlagged,
      DeletionStep.similar => const <String>{},
      DeletionStep.video => videoFlagged,
    };
    final flaggedImages = wizard.orderedImages
        .where((image) => flaggedIds.contains(image.id))
        .toList();
    if (step == DeletionStep.junk) {
      // Most-confident junk first so the clearest cuts are reviewed up top.
      flaggedImages.sort((a, b) {
        final ca = wizard.junkFlags[a.id]?.confidence ?? 0;
        final cb = wizard.junkFlags[b.id]?.confidence ?? 0;
        return cb.compareTo(ca);
      });
    } else if (step == DeletionStep.video) {
      // Most-confident flag first, same rationale as junk.
      flaggedImages.sort((a, b) {
        final ca = wizard.videoFlags[a.id]?.confidence ?? 0;
        final cb = wizard.videoFlags[b.id]?.confidence ?? 0;
        return cb.compareTo(ca);
      });
    }

    bool marked(String id) => isMarkedForDeletion(
      plan,
      id,
      step: step,
      qualityFlagged: qualityFlagged,
      junkFlagged: junkFlagged,
      similarKeepers: const <String, String>{},
      videoFlagged: videoFlagged,
    );

    final keepImages =
        flaggedImages.where((image) => !marked(image.id)).toList();
    final deleteImages =
        flaggedImages.where((image) => marked(image.id)).toList();

    if (flaggedImages.isEmpty) {
      return Padding(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 0),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(widget.header, style: theme.textTheme.titleLarge),
            const SizedBox(height: 12),
            Expanded(
              child: Center(
                child: Text(
                  _emptyMessage(step),
                  style: theme.textTheme.bodyLarge,
                ),
              ),
            ),
          ],
        ),
      );
    }

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 8, 16, 0),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(widget.header, style: theme.textTheme.titleLarge),
              const SizedBox(height: 8),
              Text(
                'Keeping ${keepImages.length}',
                style: theme.textTheme.titleSmall?.copyWith(
                  fontWeight: FontWeight.w600,
                ),
              ),
            ],
          ),
        ),
        // The keep section gets the larger share of the slack; the delete
        // panel takes the rest when expanded, or just its header when not.
        Expanded(
          flex: 3,
          child: keepImages.isEmpty
              ? Center(
                  child: Text(
                    'Tap a photo below to keep it',
                    style: theme.textTheme.bodyLarge?.copyWith(
                      color: theme.colorScheme.onSurfaceVariant,
                    ),
                  ),
                )
              : _grid(context, wizard, keepImages, inDeleteSection: false),
        ),
        const Divider(height: 1),
        if (_deletePanelExpanded)
          Flexible(
            flex: 2,
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                _deletePanelHeader(theme, deleteImages.length),
                Expanded(
                  child: deleteImages.isEmpty
                      ? Padding(
                          padding: const EdgeInsets.all(24),
                          child: Text(
                            'Nothing marked for deletion',
                            style: theme.textTheme.bodyMedium?.copyWith(
                              color: theme.colorScheme.onSurfaceVariant,
                            ),
                          ),
                        )
                      : _grid(
                          context,
                          wizard,
                          deleteImages,
                          inDeleteSection: true,
                        ),
                ),
              ],
            ),
          )
        else
          _deletePanelHeader(theme, deleteImages.length),
      ],
    );
  }

  Widget _deletePanelHeader(ThemeData theme, int count) {
    return InkWell(
      onTap: () => setState(
        () => _deletePanelExpanded = !_deletePanelExpanded,
      ),
      child: Padding(
        padding: const EdgeInsets.fromLTRB(16, 10, 16, 10),
        child: Row(
          children: [
            Icon(
              _deletePanelExpanded ? Icons.expand_more : Icons.chevron_right,
            ),
            const SizedBox(width: 8),
            Text(
              'Deleting $count',
              style: theme.textTheme.titleSmall?.copyWith(
                fontWeight: FontWeight.w600,
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _grid(
    BuildContext context,
    Wizard wizard,
    List<ImageInfo> images, {
    required bool inDeleteSection,
  }) {
    return ImageGrid(
      count: images.length,
      builder: (context, index) {
        final image = images[index];
        return ImageCell(
          key: ValueKey('${image.id}:$inDeleteSection'),
          image: image,
          chips: _chips(wizard, image.id),
          onTap: () {
            final notifier = ref.read(deletionPlanProvider.notifier);
            if (inDeleteSection) {
              notifier.keep(image.id);
            } else {
              notifier.mark(image.id);
            }
          },
          onExpand: () => _openDetail(context, wizard, image),
        );
      },
    );
  }

  String _emptyMessage(DeletionStep step) => switch (step) {
    DeletionStep.quality => 'No blurry or poorly exposed images found',
    DeletionStep.junk => 'No screenshots, scans, or memes found',
    DeletionStep.similar => 'No similar photos found',
    DeletionStep.video =>
      'No too-short, corrupt, blurry, or static videos found',
  };

  List<Widget> _chips(Wizard wizard, String id) {
    switch (step) {
      case DeletionStep.quality:
        final flag = wizard.qualityFlags[id];
        if (flag == null) {
          return const <Widget>[];
        }
        return flag.reasons.map((reason) => ReasonChip(reason.label)).toList();
      case DeletionStep.junk:
        final flag = wizard.junkFlags[id];
        if (flag == null) {
          return const <Widget>[];
        }
        final pct = (flag.confidence * 100).round();
        return <Widget>[ReasonChip('${flag.reason} · $pct%')];
      case DeletionStep.similar:
        return const <Widget>[];
      case DeletionStep.video:
        final flag = wizard.videoFlags[id];
        if (flag == null) {
          return const <Widget>[];
        }
        final pct = (flag.confidence * 100).round();
        return <Widget>[ReasonChip('${_videoReasonLabel(flag.reason)} · $pct%')];
    }
  }

  /// "too_short" -> "Too short"; junk categories ("screenshot") pass through.
  String _videoReasonLabel(String reason) {
    final spaced = reason.replaceAll('_', ' ');
    if (spaced.isEmpty) {
      return spaced;
    }
    return spaced[0].toUpperCase() + spaced.substring(1);
  }

  void _openDetail(BuildContext context, Wizard wizard, ImageInfo image) {
    final quality = wizard.qualityFlags[image.id];
    final junk = wizard.junkFlags[image.id];
    final video = wizard.videoFlags[image.id];
    showImageDetail(
      context,
      image: image,
      canToggleDeletion: true,
      step: step,
      qualityFlagged: wizard.qualityFlags.keys.toSet(),
      junkFlagged: wizard.junkFlags.keys.toSet(),
      videoFlagged: wizard.videoFlags.keys.toSet(),
      sharpness: quality?.sharpness,
      exposureScore: quality?.exposureScore,
      junkReason: junk?.reason,
      junkConfidence: junk?.confidence,
      videoReason: video?.reason,
      videoConfidence: video?.confidence,
    );
  }
}
