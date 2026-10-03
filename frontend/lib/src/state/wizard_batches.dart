part of 'wizard.dart';

/// Batch menu: batches, scoped runs and the per-batch pass lifecycle.
extension WizardBatches on Wizard {
  /// True once the Organize stage is done and passes run per batch.
  bool get batchMode => _batchMode;

  BatchInfo? _batchByKey(String? key) {
    if (key == null) {
      return null;
    }
    for (final batch in _batches) {
      if (batch.key == key) {
        return batch;
      }
    }
    return null;
  }

  /// Ids of the batch a pass is running or being reviewed for, or null when no
  /// batch is active (reviews then show everything).
  Set<String>? get reviewScope {
    return _batchByKey(_activeBatchKey)?.imageIds.toSet();
  }

  /// Scope sent to the back end for the active batch (empty = whole session).
  List<String> get _scopeIds =>
      _batchByKey(_activeBatchKey)?.imageIds ?? const <String>[];

  /// Back-end `batch_key` for the active batch ('' = none).
  String get _activeBatchKeyArg => _activeBatchKey ?? '';

  /// Similar groups with a member in the active batch (all groups when no
  /// batch is active).
  List<SimilarGroupInfo> get reviewSimilarGroups {
    final scope = reviewScope;
    if (scope == null) {
      return similarGroups;
    }
    return _similarGroups
        .where((group) => group.memberIds.any(scope.contains))
        .toList(growable: false);
  }

  int _scopeCount() => reviewScope?.length ?? _images.length;

  int _inScopeCount(Iterable<String> ids) {
    final scope = reviewScope;
    return scope == null ? ids.length : ids.where(scope.contains).length;
  }

  /// Builds the batches from the effective trip folders (plus the photos in no
  /// trip) and opens the batch menu.
  void _enterBatchMode() {
    _batches.clear();
    for (final folder in tripFolders) {
      final ids = <String>{};
      for (final trip in folder.trips) {
        ids.addAll(trip.memberIds);
      }
      if (ids.isEmpty) {
        continue;
      }
      _batches.add(
        BatchInfo(
          key: folder.name.isEmpty ? '__no-folder__' : folder.name,
          title: folder.name.isEmpty ? 'Other' : folder.name,
          imageIds: _orderedIds.where(ids.contains).toList(growable: false),
        ),
      );
    }
    final unassigned = unassignedTripImageIds;
    if (unassigned.isNotEmpty) {
      _batches.add(
        BatchInfo(
          key: Wizard.kUnassignedBatchKey,
          title: 'Unassigned',
          imageIds: unassigned,
        ),
      );
    }
    _batchMode = true;
    _activeBatchKey = null;
    _runAllQueue.clear();
    _runAllStep = null;
    _selectedBatchKey = _batches.isEmpty ? null : _batches.first.key;
  }

  WizardBatchMenu get _batchMenuPhase {
    return WizardBatchMenu(
      batches: [for (final batch in _batches) _summarize(batch)],
      selectedKey: _selectedBatchKey,
      markedCount: _deletedImageIds().length,
      totalImages: _images.length,
    );
  }

  bool _passDone(WizardStep step, String batchKey) =>
      _sessionPassDone.contains(step.index) ||
      _batchPassDone.contains('${step.index}:$batchKey');

  BatchSummary _summarize(BatchInfo batch) {
    final ids = batch.imageIds.toSet();
    var videos = 0;
    for (final id in ids) {
      if (_images[id]?.isVideo ?? false) {
        videos++;
      }
    }
    int flagged(Iterable<String> flaggedIds) =>
        flaggedIds.where(ids.contains).length;
    BatchPassStatus status(WizardStep step, int flaggedCount) =>
        BatchPassStatus(
          done: _passDone(step, batch.key),
          flagged: flaggedCount,
        );
    return BatchSummary(
      key: batch.key,
      title: batch.title,
      photoCount: ids.length - videos,
      videoCount: videos,
      passes: {
        WizardStep.quality.index: status(
          WizardStep.quality,
          flagged(_qualityFlags.keys),
        ),
        WizardStep.duplicates.index: status(
          WizardStep.duplicates,
          _similarGroups
              .where((group) => group.memberIds.any(ids.contains))
              .length,
        ),
        WizardStep.junk.index: status(
          WizardStep.junk,
          flagged(_junkFlags.keys),
        ),
        WizardStep.video.index: status(
          WizardStep.video,
          flagged(_videoFlags.keys),
        ),
      },
    );
  }

  void selectBatch(String key) {
    if (_state.value is! WizardBatchMenu || _batchByKey(key) == null) {
      return;
    }
    _selectedBatchKey = key;
    _state = AsyncValue.data(_batchMenuPhase);
  }

  /// Back to the menu from a pass or review, dropping the active batch.
  void _returnToBatchMenu() {
    _activeBatchKey = null;
    _runAllQueue.clear();
    _runAllStep = null;
    if (_batchMode) {
      _state = AsyncValue.data(_batchMenuPhase);
    } else {
      _state = AsyncValue.data(_returnPhase ?? const WizardStart());
    }
  }

  /// Runs [step] on the selected batch; its review opens when it finishes.
  void startBatchPass(WizardStep step) {
    if (_state.value is! WizardBatchMenu || _selectedBatchKey == null) {
      return;
    }
    _runAllQueue.clear();
    _runAllStep = null;
    _activeBatchKey = _selectedBatchKey;
    _beginPass(step);
  }

  /// Runs [step] on every batch that has not finished it, one after another,
  /// then returns to the menu.
  void startPassOnAllBatches(WizardStep step) {
    if (_state.value is! WizardBatchMenu) {
      return;
    }
    final pending = [
      for (final batch in _batches)
        if (!_passDone(step, batch.key)) batch.key,
    ];
    if (pending.isEmpty) {
      return;
    }
    _runAllStep = step;
    _runAllQueue
      ..clear()
      ..addAll(pending.skip(1));
    _activeBatchKey = pending.first;
    _beginPass(step);
  }

  /// Opens the review for a pass the selected batch already finished.
  void reviewBatchPass(WizardStep step) {
    if (_state.value is! WizardBatchMenu || _selectedBatchKey == null) {
      return;
    }
    _activeBatchKey = _selectedBatchKey;
    _state = AsyncValue.data(switch (step) {
      WizardStep.quality => _qualityReviewPhase,
      WizardStep.duplicates => _similarReviewPhase,
      WizardStep.junk => _junkReviewPhase,
      _ => _videoReviewPhase,
    });
  }

  void _beginPass(WizardStep step) {
    final client = _ref.read(kustaviClientProvider).requireValue;
    switch (step) {
      case WizardStep.quality:
        _startQualityPass();
      case WizardStep.duplicates:
        // Groups are recomputed, so the batch's old ones are dropped.
        final scope = reviewScope;
        _similarGroups.removeWhere(
          (group) => scope == null || group.memberIds.any(scope.contains),
        );
        _state = const AsyncValue.data(WizardSimilarRunning());
        _subscribe(
          client.runSimilarPass(
            skipImageIds: _deletedBeforeSimilar(),
            scopeImageIds: _scopeIds,
            batchKey: _activeBatchKeyArg,
          ),
          _onSimilarEvent,
          _onSimilarDone,
        );
      case WizardStep.junk:
        // Images the pass already classified are not re-emitted, so existing
        // flags are kept.
        if (_modelReady) {
          _startJunkPass();
        } else {
          _state = const AsyncValue.data(WizardJunkPrep());
        }
      case WizardStep.video:
        _state = const AsyncValue.data(WizardVideoRunning());
        _subscribe(
          client.runVideoPass(
            skipVideoIds: _deletedBeforeVideo(),
            scopeImageIds: _scopeIds,
            batchKey: _activeBatchKeyArg,
          ),
          _onVideoEvent,
          _onVideoDone,
        );
      case WizardStep.select || WizardStep.trips || WizardStep.copy:
        break;
    }
  }

  /// A pass finished for the active batch: record it, then either start the
  /// next queued batch, return to the menu (end of a run-all), or open the
  /// pass's review.
  void _finishBatchRun(WizardStep step, WizardPhase Function() review) {
    final key = _activeBatchKey;
    if (key != null) {
      _batchPassDone.add('${step.index}:$key');
    }
    if (_runAllStep == step) {
      if (_runAllQueue.isNotEmpty) {
        _activeBatchKey = _runAllQueue.removeAt(0);
        _beginPass(step);
      } else {
        _returnToBatchMenu();
      }
      return;
    }
    _state = AsyncValue.data(review());
  }

  /// [Done] on any batch review: back to the menu.
  void closeBatchReview() {
    final phase = _state.value;
    if (phase is WizardQualityReview ||
        phase is WizardSimilarReview ||
        phase is WizardJunkReview ||
        phase is WizardVideoReview) {
      _returnToBatchMenu();
    }
  }

  /// Menu -> final review of everything marked for deletion.
  void continueFromBatches() {
    if (_state.value is! WizardBatchMenu) {
      return;
    }
    _activeBatchKey = null;
    _state = AsyncValue.data(_deletionReviewPhase);
  }

  /// Menu -> back to the trip folders to regroup. Batch progress marks are
  /// dropped because the batches are rebuilt from the edited folders.
  void reopenOrganize() {
    if (_state.value is! WizardBatchMenu) {
      return;
    }
    _batchMode = false;
    _batchPassDone.clear();
    _sessionPassDone.clear();
    _activeBatchKey = null;
    _state = AsyncValue.data(_tripsReviewPhase);
  }

  /// [ids] limited to the active batch (all of them when none is active).
  List<String> _flaggedInScope(Iterable<String> ids) {
    final scope = reviewScope;
    return scope == null
        ? ids.toList(growable: false)
        : ids.where(scope.contains).toList(growable: false);
  }
}
