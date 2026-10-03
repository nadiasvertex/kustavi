part of 'wizard.dart';

/// Quality, junk, duplicate and video passes: events, reviews and thresholds.
extension WizardPasses on Wizard {
  // Public accessors for the UI (quality review screen)
  double get blurThreshold => _blurThreshold;
  double get underexposedThreshold => _underexposedThreshold;
  double get overexposedThreshold => _overexposedThreshold;

  WizardQualityReview get _qualityReviewPhase => WizardQualityReview(
    flaggedCount: _inScopeCount(_qualityFlags.keys),
    totalImages: _scopeCount(),
    rerunEnabled: _hasThresholdChanges,
    previewFlagged: _previewFlagged,
  );

  /// Asks the back end how many photos the current sliders would flag. Only
  /// the newest request may publish, so a slow answer for an earlier slider
  /// position cannot overwrite a later one.
  Future<void> _refreshQualityPreview() async {
    final seq = ++_previewSeq;
    final client = _ref.read(kustaviClientProvider).value;
    if (client == null) {
      return;
    }
    try {
      final response = await client.previewQualityThresholds(
        blurThreshold: _blurThreshold,
        underexposedThreshold: _underexposedThreshold,
        overexposedThreshold: _overexposedThreshold,
        scopeImageIds: _scopeIds,
      );
      if (seq != _previewSeq) {
        return;
      }
      _previewFlagged = response.flagged;
      _publishQualityReviewPhase();
    } on Object {
      // The preview is advisory; leave the previous count in place.
    }
  }

  bool get _hasThresholdChanges {
    if (!_hasLastRunThresholds) {
      return false;
    }
    return _blurThreshold != _lastBlurThreshold ||
        _underexposedThreshold != _lastUnderexposedThreshold ||
        _overexposedThreshold != _lastOverexposedThreshold;
  }

  WizardJunkReview get _junkReviewPhase => WizardJunkReview(
    flaggedCount: _inScopeCount(_junkFlags.keys),
    totalImages: _scopeCount(),
  );

  void _onQualityEvent(pb.QualityEvent event) {
    if (_state.value is! WizardQualityRunning) {
      return;
    }
    switch (event.whichEvent()) {
      case pb.QualityEvent_Event.progress:
        _state = AsyncValue.data(
          WizardQualityRunning(
            done: event.progress.done,
            total: event.progress.total,
          ),
        );
      case pb.QualityEvent_Event.flag:
        final flag = QualityFlagInfo.fromFlag(event.flag);
        _qualityFlags[flag.imageId] = flag;
      case pb.QualityEvent_Event.complete:
        break;
      case pb.QualityEvent_Event.notSet:
        break;
    }
  }

  void _onQualityDone() {
    if (_state.value is! WizardQualityRunning) {
      return;
    }
    _saveLastRunThresholds();
    _previewFlagged = null;
    _finishBatchRun(WizardStep.quality, () => _qualityReviewPhase);
  }

  void _saveLastRunThresholds() {
    _hasLastRunThresholds = true;
    _lastBlurThreshold = _blurThreshold;
    _lastUnderexposedThreshold = _underexposedThreshold;
    _lastOverexposedThreshold = _overexposedThreshold;
  }

  void cancelQuality() {
    if (_state.value is! WizardQualityRunning) {
      return;
    }
    _cancelPass();
    _returnToBatchMenu();
  }

  void rerunQualityPass() {
    if (_state.value is! WizardQualityReview || !_hasThresholdChanges) {
      return;
    }
    _startQualityPass();
  }

  /// Runs the quality pass over the active batch with the current sliders.
  /// Its flags are recomputed, so the batch's old ones are dropped first.
  void _startQualityPass() {
    final scope = reviewScope;
    _qualityFlags.removeWhere((id, _) => scope == null || scope.contains(id));
    _saveLastRunThresholds();
    _state = const AsyncValue.data(WizardQualityRunning());
    final client = _ref.read(kustaviClientProvider).requireValue;
    _subscribe(
      client.runQualityPass(
        blurThreshold: _blurThreshold,
        underexposedThreshold: _underexposedThreshold,
        overexposedThreshold: _overexposedThreshold,
        scopeImageIds: _scopeIds,
        batchKey: _activeBatchKeyArg,
      ),
      _onQualityEvent,
      _onQualityDone,
    );
  }

  void setBlurThreshold(double value) {
    if (value == _blurThreshold) {
      return;
    }
    _blurThreshold = value;
    _publishQualityReviewPhase();
    unawaited(_refreshQualityPreview());
  }

  void setUnderexposedThreshold(double value) {
    if (value == _underexposedThreshold) {
      return;
    }
    _underexposedThreshold = value;
    _publishQualityReviewPhase();
    unawaited(_refreshQualityPreview());
  }

  void setOverexposedThreshold(double value) {
    if (value == _overexposedThreshold) {
      return;
    }
    _overexposedThreshold = value;
    _publishQualityReviewPhase();
    unawaited(_refreshQualityPreview());
  }

  void resetThresholds() {
    if (_blurThreshold == Wizard._kDefaultBlurThreshold &&
        _underexposedThreshold == Wizard._kDefaultUnderexposedThreshold &&
        _overexposedThreshold == Wizard._kDefaultOverexposedThreshold) {
      return;
    }
    _blurThreshold = Wizard._kDefaultBlurThreshold;
    _underexposedThreshold = Wizard._kDefaultUnderexposedThreshold;
    _overexposedThreshold = Wizard._kDefaultOverexposedThreshold;
    _publishQualityReviewPhase();
    unawaited(_refreshQualityPreview());
  }

  /// Republishes the quality review phase with the current threshold state
  /// (recomputing [WizardQualityReview.rerunEnabled]). [WizardPhase] is
  /// immutable and Riverpod only notifies listeners when the state value
  /// differs, so each changed threshold must produce a *new* phase instance
  /// — reassigning the same instance is a silent no-op and the review
  /// screen would never rebuild.
  void _publishQualityReviewPhase() {
    if (_state.value is WizardQualityReview) {
      _state = AsyncValue.data(_qualityReviewPhase);
    }
  }

  void keepAllQualityFlagged() {
    if (_state.value is! WizardQualityReview) {
      return;
    }
    _ref
        .read(deletionPlanProvider.notifier)
        .keepAll(_flaggedInScope(_qualityFlags.keys));
  }

  void markAllQualityFlagged() {
    if (_state.value is! WizardQualityReview) {
      return;
    }
    _ref
        .read(deletionPlanProvider.notifier)
        .markAll(_flaggedInScope(_qualityFlags.keys));
  }

  bool get _modelReady {
    return _ref.read(modelStatusProvider).value is ModelPrepReady;
  }

  void cancelJunkPrep() {
    if (_state.value is! WizardJunkPrep) {
      return;
    }
    _ref.read(modelStatusProvider.notifier).cancelDownload();
    _returnToBatchMenu();
  }

  void _startJunkPass() {
    _junkProfileStart = null;
    _junkProfileBaseDone = null;
    _junkLastEventAt = null;
    _junkLastDone = 0;
    _state = const AsyncValue.data(WizardJunkRunning());
    final client = _ref.read(kustaviClientProvider).requireValue;
    _subscribe(
      client.runJunkPass(
        skipImageIds: _deletedBeforeJunk(),
        scopeImageIds: _scopeIds,
        batchKey: _activeBatchKeyArg,
      ),
      _onJunkEvent,
      _onJunkDone,
    );
  }

  WizardSimilarReview get _similarReviewPhase => WizardSimilarReview(
    groupCount: reviewSimilarGroups.length,
    markedCount: _similarMarkedCount(),
  );

  void _onSimilarDone() {
    if (_state.value is! WizardSimilarRunning) {
      return;
    }
    _finishBatchRun(WizardStep.duplicates, () => _similarReviewPhase);
  }

  void _onJunkEvent(pb.JunkEvent event) {
    if (_state.value is! WizardJunkRunning) {
      return;
    }
    switch (event.whichEvent()) {
      case pb.JunkEvent_Event.progress:
        final done = event.progress.done;
        final total = event.progress.total;
        final now = DateTime.now();

        // Begin (or extend) the profile once an event follows a real gap —
        // i.e. the vision model actually spent time on an image.
        if (_junkLastEventAt != null &&
            now.difference(_junkLastEventAt!).inMilliseconds >= 250) {
          _junkProfileStart ??= _junkLastEventAt;
          _junkProfileBaseDone ??= _junkLastDone;
        }
        _junkLastEventAt = now;
        _junkLastDone = done;

        double? secondsPerImage;
        DateTime? estimatedCompletion;
        if (_junkProfileStart != null && _junkProfileBaseDone != null) {
          final measured = done - _junkProfileBaseDone!;
          final elapsedMs = now.difference(_junkProfileStart!).inMilliseconds;
          if (measured > 0 && elapsedMs > 0) {
            secondsPerImage = elapsedMs / 1000 / measured;
            final remaining = total - done;
            estimatedCompletion = remaining > 0
                ? now.add(
                    Duration(
                      milliseconds: (secondsPerImage * remaining * 1000)
                          .round(),
                    ),
                  )
                : now;
          }
        }

        _state = AsyncValue.data(
          WizardJunkRunning(
            done: done,
            total: total,
            secondsPerImage: secondsPerImage,
            estimatedCompletion: estimatedCompletion,
          ),
        );
      case pb.JunkEvent_Event.flag:
        final flag = JunkFlagInfo.fromFlag(event.flag);
        _junkFlags[flag.imageId] = flag;
      case pb.JunkEvent_Event.complete:
        break;
      case pb.JunkEvent_Event.notSet:
        break;
    }
  }

  void _onJunkDone() {
    if (_state.value is! WizardJunkRunning) {
      return;
    }
    _finishBatchRun(WizardStep.junk, () => _junkReviewPhase);
  }

  void cancelJunk() {
    if (_state.value is! WizardJunkRunning) {
      return;
    }
    _cancelPass();
    _returnToBatchMenu();
  }

  void keepAllJunkFlagged() {
    if (_state.value is! WizardJunkReview) {
      return;
    }
    _ref
        .read(deletionPlanProvider.notifier)
        .keepAll(_flaggedInScope(_junkFlags.keys));
  }

  void markAllJunkFlagged() {
    if (_state.value is! WizardJunkReview) {
      return;
    }
    _ref
        .read(deletionPlanProvider.notifier)
        .markAll(_flaggedInScope(_junkFlags.keys));
  }

  void _onSimilarEvent(pb.SimilarEvent event) {
    if (_state.value is! WizardSimilarRunning) {
      return;
    }
    switch (event.whichEvent()) {
      case pb.SimilarEvent_Event.progress:
        _state = AsyncValue.data(
          WizardSimilarRunning(
            done: event.progress.done,
            total: event.progress.total,
          ),
        );
      case pb.SimilarEvent_Event.group:
        _similarGroups.add(SimilarGroupInfo.fromGroup(event.group));
      case pb.SimilarEvent_Event.complete:
        break;
      case pb.SimilarEvent_Event.notSet:
        break;
    }
  }

  int _similarMarkedCount() {
    final plan = _ref.read(deletionPlanProvider);
    final keepers = similarKeeperMap(plan, _similarGroups);
    return reviewSimilarGroups
        .expand((group) => group.memberIds)
        .where(
          (id) => isMarkedForDeletion(
            plan,
            id,
            step: DeletionStep.similar,
            qualityFlagged: _qualityFlags.keys.toSet(),
            junkFlagged: _junkFlags.keys.toSet(),
            similarKeepers: keepers,
          ),
        )
        .length;
  }

  void cancelSimilar() {
    if (_state.value is! WizardSimilarRunning) {
      return;
    }
    _cancelPass();
    _returnToBatchMenu();
  }

  void _onVideoEvent(pb.VideoEvent event) {
    if (_state.value is! WizardVideoRunning) {
      return;
    }
    switch (event.whichEvent()) {
      case pb.VideoEvent_Event.progress:
        _videoTotal = event.progress.total;
        _state = AsyncValue.data(
          WizardVideoRunning(
            done: event.progress.done,
            total: event.progress.total,
          ),
        );
      case pb.VideoEvent_Event.flag:
        final flag = VideoFlagInfo.fromFlag(event.flag);
        _videoFlags[flag.videoId] = flag;
      case pb.VideoEvent_Event.complete:
        break;
      case pb.VideoEvent_Event.notSet:
        break;
    }
  }

  void _onVideoDone() {
    if (_state.value is! WizardVideoRunning) {
      return;
    }
    _finishBatchRun(WizardStep.video, () => _videoReviewPhase);
  }

  void cancelVideo() {
    if (_state.value is! WizardVideoRunning) {
      return;
    }
    _cancelPass();
    _returnToBatchMenu();
  }

  WizardVideoReview get _videoReviewPhase {
    final scope = reviewScope;
    final totalVideos = scope == null
        ? _videoTotal
        : scope.where((id) => _images[id]?.isVideo ?? false).length;
    return WizardVideoReview(
      flaggedCount: _inScopeCount(_videoFlags.keys),
      totalVideos: totalVideos,
    );
  }

  void keepAllVideoFlagged() {
    if (_state.value is! WizardVideoReview) {
      return;
    }
    _ref
        .read(deletionPlanProvider.notifier)
        .keepAll(_flaggedInScope(_videoFlags.keys));
  }

  void markAllVideoFlagged() {
    if (_state.value is! WizardVideoReview) {
      return;
    }
    _ref
        .read(deletionPlanProvider.notifier)
        .markAll(_flaggedInScope(_videoFlags.keys));
  }
}
