part of 'wizard.dart';

/// Commit: destination, summary and the copy stream.
extension WizardCommit on Wizard {
  /// The suggested default destination: a sibling of the source folder named
  /// `<source-name>-kept` (spec/frontend.md §6.2 S11, §15).
  String _suggestedDestination() {
    if (_sourceFolder.isEmpty) {
      return '';
    }
    final normalized = p.normalize(_sourceFolder);
    final parent = p.dirname(normalized);
    final name = p.basename(normalized);
    if (name.isEmpty) {
      return '';
    }
    return p.join(parent, '$name-kept');
  }

  /// The destination that a commit would use: the user's field value, or the
  /// suggested default when they have not typed one.
  String get _effectiveCommitDestination => _commitDestination.isNotEmpty
      ? _commitDestination
      : _suggestedDestination();

  WizardCommitSummary get _commitSummaryPhase {
    final keepIds = _keepIds();
    var keepBytes = 0;
    for (final id in keepIds) {
      keepBytes += _images[id]?.sizeBytes ?? 0;
    }
    return WizardCommitSummary(
      keepCount: keepIds.length,
      keepBytes: keepBytes,
      leftBehindCount: _images.length - keepIds.length,
      destination: _effectiveCommitDestination,
      estimate: _commitEstimateFor == _effectiveCommitDestination
          ? _commitEstimate
          : null,
    );
  }

  /// The commit request for [destination]. The destination is treated as a
  /// library that may already hold some of these photos.
  pb.CommitRequest _commitRequest(String destination, List<String> keepIds) {
    return pb.CommitRequest(
      destination: destination,
      keepIds: keepIds,
      folderForId: commitFolderPlan().entries,
      mergeExisting: true,
    );
  }

  /// Asks the back end how much a commit to the current destination would
  /// write and whether it fits. Called when the summary opens and, debounced,
  /// as the destination field changes. A stale answer is dropped.
  void refreshCommitEstimate({
    Duration delay = const Duration(milliseconds: 400),
  }) {
    _estimateTimer?.cancel();
    final destination = _effectiveCommitDestination;
    if (_state.value is! WizardCommitSummary || destination.trim().isEmpty) {
      return;
    }
    _estimateTimer = Timer(delay, () async {
      final client = _ref.read(kustaviClientProvider).requireValue;
      try {
        final estimate = await client.estimateCommit(
          _commitRequest(destination, _keepIds()),
        );
        if (_state.value is! WizardCommitSummary ||
            destination != _effectiveCommitDestination) {
          return;
        }
        _commitEstimate = estimate;
        _commitEstimateFor = destination;
        _state = AsyncValue.data(_commitSummaryPhase);
      } on Object {
        // An unreadable estimate leaves [Copy] disabled; the user can retry
        // by editing the destination.
      }
    });
  }

  /// S11 destination field edit. Republishes the summary so the shell's
  /// [Copy] button re-evaluates its enabled state (see the note on
  /// [_publishQualityReviewPhase] for why a fresh instance is required).
  void setCommitDestination(String value) {
    if (_state.value is! WizardCommitSummary || value == _commitDestination) {
      return;
    }
    _commitDestination = value;
    _state = AsyncValue.data(_commitSummaryPhase);
    refreshCommitEstimate();
  }

  /// S11 [Back] -> trips review.
  void backFromCommitSummary() {
    if (_state.value is! WizardCommitSummary) {
      return;
    }
    if (_batchMode) {
      _state = AsyncValue.data(_deletionReviewPhase);
    } else {
      _state = AsyncValue.data(_returnPhase ?? _tripsReviewPhase);
    }
  }

  /// S11 [Copy] -> run the commit pass (S12).
  void startCommit() {
    if (_state.value is! WizardCommitSummary) {
      return;
    }
    final destination = _effectiveCommitDestination;
    if (destination.isEmpty) {
      return;
    }
    _returnPhase = _commitSummaryPhase;
    _committedDestination = destination;
    _commitKeepIds = _keepIds();
    _commitTotalBytes = _commitKeepIds.fold<int>(
      0,
      (sum, id) => sum + (_images[id]?.sizeBytes ?? 0),
    );
    _commitCopied = 0;
    _commitSkipped = 0;
    _commitAlreadyPresent = 0;
    _commitErrors = const <String>[];
    _state = AsyncValue.data(
      WizardCommitting(
        total: _commitKeepIds.length,
        totalBytes: _commitTotalBytes,
      ),
    );
    final client = _ref.read(kustaviClientProvider).requireValue;
    final request = _commitRequest(destination, _commitKeepIds);
    _subscribe(client.commit(request), _onCommitEvent, _onCommitDone);
  }

  void cancelCommit() {
    if (_state.value is! WizardCommitting) {
      return;
    }
    _cancelPass();
    _state = AsyncValue.data(_returnPhase ?? _commitSummaryPhase);
  }

  void _onCommitEvent(pb.CommitEvent event) {
    if (_state.value is! WizardCommitting) {
      return;
    }
    switch (event.whichEvent()) {
      case pb.CommitEvent_Event.progress:
        final done = event.progress.done;
        // CommitProgress carries only file counts; approximate bytes-done by
        // summing the sizes of the first `done` ids in the keep set.
        var doneBytes = 0;
        for (var i = 0; i < done && i < _commitKeepIds.length; i++) {
          doneBytes += _images[_commitKeepIds[i]]?.sizeBytes ?? 0;
        }
        _state = AsyncValue.data(
          WizardCommitting(
            done: done,
            total: event.progress.total,
            currentName: event.progress.currentName,
            doneBytes: doneBytes,
            totalBytes: _commitTotalBytes,
          ),
        );
      case pb.CommitEvent_Event.complete:
        _commitCopied = event.complete.copied;
        _commitSkipped = event.complete.skipped;
        _commitAlreadyPresent = event.complete.alreadyPresent;
        _commitErrors = List<String>.unmodifiable(event.complete.errors);
      case pb.CommitEvent_Event.notSet:
        break;
    }
  }

  void _onCommitDone() {
    if (_state.value is! WizardCommitting) {
      return;
    }
    _state = AsyncValue.data(
      WizardDone(
        copiedCount: _commitCopied,
        skippedCount: _commitSkipped,
        alreadyPresentCount: _commitAlreadyPresent,
        destination: _committedDestination,
        errors: _commitErrors,
      ),
    );
  }
}
