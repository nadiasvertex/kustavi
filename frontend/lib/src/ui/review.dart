import 'package:flutter/material.dart' hide ImageInfo;
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../state/decisions.dart';
import '../state/domain.dart';
import '../state/wizard.dart';
import 'widgets/badges.dart';
import 'widgets/detail_view.dart';
import 'widgets/image_cell.dart';
import 'widgets/image_grid.dart';

/// Flagged-candidate review shared by S4 (quality) and S7 (junk)
/// (spec/frontend.md §6.2). Deletion toggles are on by default for
/// flagged images; cells and the detail view reflect the live
/// deletion hierarchy.
class FlaggedReview extends ConsumerWidget {
  const FlaggedReview({super.key, required this.step, required this.header});

  final DeletionStep step;
  final String header;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
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
    final flaggedImages = wizard
        .orderedImages
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
    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 8, 16, 0),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(header, style: theme.textTheme.titleLarge),
          const SizedBox(height: 12),
          if (flaggedImages.isEmpty)
            Expanded(
              child: Center(
                child: Text(
                  _emptyMessage(step),
                  style: theme.textTheme.bodyLarge,
                ),
              ),
            )
          else
            Expanded(
              child: ImageGrid(
                count: flaggedImages.length,
                builder: (context, index) {
                  final image = flaggedImages[index];
                  return ImageCell(
                    image: image,
                    chips: _chips(wizard, image.id),
                    marked: isMarkedForDeletion(
                      plan,
                      image.id,
                      step: step,
                      qualityFlagged: qualityFlagged,
                      junkFlagged: junkFlagged,
                      similarKeepers: const <String, String>{},
                      videoFlagged: videoFlagged,
                    ),
                    onTap: () => _openDetail(context, wizard, image),
                  );
                },
              ),
            ),
        ],
      ),
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
        return flag.reasons
            .map((reason) => ReasonChip(reason.label))
            .toList();
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
