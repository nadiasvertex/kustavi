part of 'wizard.dart';

/// Deletion model: what is marked, why, and the final review.
extension WizardDeletion on Wizard {
  /// Final review -> back to the batch menu.
  void backFromDeletionReview() {
    if (_state.value is! WizardDeletionReview) {
      return;
    }
    _state = AsyncValue.data(_batchMenuPhase);
  }

  /// Final review -> commit summary.
  void continueFromDeletionReview() {
    if (_state.value is! WizardDeletionReview) {
      return;
    }
    _state = AsyncValue.data(_commitSummaryPhase);
  }

  WizardDeletionReview get _deletionReviewPhase =>
      WizardDeletionReview(markedCount: _deletedImageIds().length);

  /// Photos marked for deletion, grouped by batch in batch order. Batches with
  /// nothing marked are left out.
  List<({String title, List<ImageInfo> images})> get markedByBatch {
    final deleted = _deletedImageIds();
    final groups = <({String title, List<ImageInfo> images})>[];
    for (final batch in _batches) {
      final images = [
        for (final id in batch.imageIds)
          if (deleted.contains(id) && _images[id] != null) _images[id]!,
      ];
      if (images.isNotEmpty) {
        groups.add((title: batch.title, images: images));
      }
    }
    return groups;
  }

  /// Why [id] is marked for deletion, one label per pass that flagged it.
  /// A photo the user marked by hand is labelled "Marked by you".
  List<String> deletionReasons(String id) {
    final reasons = <String>[];
    final quality = _qualityFlags[id];
    if (quality != null) {
      reasons.addAll(quality.reasons.map((r) => r.label));
    }
    final junk = _junkFlags[id];
    if (junk != null) {
      reasons.add(junk.reason);
    }
    final video = _videoFlags[id];
    if (video != null) {
      final spaced = video.reason.replaceAll('_', ' ');
      reasons.add(
        spaced.isEmpty ? spaced : spaced[0].toUpperCase() + spaced.substring(1),
      );
    }
    final keepers = similarKeeperMap(
      _ref.read(deletionPlanProvider),
      _similarGroups,
    );
    final keeper = keepers[id];
    if (keeper != null && keeper.isNotEmpty && keeper != id) {
      reasons.add('Duplicate');
    }
    if (_ref.read(deletionPlanProvider).isExplicitlyDeleted(id)) {
      reasons.add('Marked by you');
    }
    return reasons;
  }

  /// Ids marked for deletion by the quality step, so the duplicate pass never
  /// scores them or picks them as a group keeper.
  List<String> _deletedBeforeSimilar() {
    final plan = _ref.read(deletionPlanProvider);
    final qualityFlagged = _qualityFlags.keys.toSet();
    bool marked(String id) => isMarkedForDeletion(
      plan,
      id,
      step: DeletionStep.quality,
      qualityFlagged: qualityFlagged,
      junkFlagged: const <String>{},
      similarKeepers: const <String, String>{},
    );
    return _images.keys.where(marked).toList(growable: false);
  }

  /// Ids already marked for deletion by an earlier step, so the video pass
  /// can skip analysis on them.
  List<String> _deletedBeforeVideo() {
    final plan = _ref.read(deletionPlanProvider);
    final keepers = similarKeeperMap(plan, _similarGroups);
    final qualityFlagged = _qualityFlags.keys.toSet();
    final junkFlagged = _junkFlags.keys.toSet();
    bool marked(String id) => DeletionStep.values
        .where((step) => step != DeletionStep.video)
        .any(
          (step) => isMarkedForDeletion(
            plan,
            id,
            step: step,
            qualityFlagged: qualityFlagged,
            junkFlagged: junkFlagged,
            similarKeepers: keepers,
          ),
        );
    return _images.keys.where(marked).toList(growable: false);
  }

  /// Ids already marked for deletion by the quality or duplicates step, so
  /// the junk pass can skip inference on them.
  List<String> _deletedBeforeJunk() {
    final plan = _ref.read(deletionPlanProvider);
    final keepers = similarKeeperMap(plan, _similarGroups);
    final qualityFlagged = _qualityFlags.keys.toSet();
    bool marked(String id) =>
        isMarkedForDeletion(
          plan,
          id,
          step: DeletionStep.quality,
          qualityFlagged: qualityFlagged,
          junkFlagged: const <String>{},
          similarKeepers: keepers,
        ) ||
        isMarkedForDeletion(
          plan,
          id,
          step: DeletionStep.similar,
          qualityFlagged: qualityFlagged,
          junkFlagged: const <String>{},
          similarKeepers: keepers,
        );
    return _images.keys.where(marked).toList(growable: false);
  }

  /// Image ids marked for deletion by any step (quality, junk, similar) or
  /// explicitly by the user. These are excluded from the trips panel and from
  /// the commit copy set.
  Set<String> _deletedImageIds() {
    final plan = _ref.read(deletionPlanProvider);
    final keepers = similarKeeperMap(plan, _similarGroups);
    final qualityFlagged = _qualityFlags.keys.toSet();
    final junkFlagged = _junkFlags.keys.toSet();
    final videoFlagged = _videoFlags.keys.toSet();
    bool deleted(String id) => DeletionStep.values.any(
      (step) => isMarkedForDeletion(
        plan,
        id,
        step: step,
        qualityFlagged: qualityFlagged,
        junkFlagged: junkFlagged,
        similarKeepers: keepers,
        videoFlagged: videoFlagged,
      ),
    );
    return _images.keys.where(deleted).toSet();
  }

  /// Image ids to copy at commit: every scanned image not marked for deletion
  /// by any step, in scan order.
  List<String> _keepIds() {
    final deleted = _deletedImageIds();
    return _orderedIds
        .where((id) => !deleted.contains(id))
        .toList(growable: false);
  }
}
