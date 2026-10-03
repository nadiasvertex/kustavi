part of 'wizard.dart';

/// Session: folder selection, scan, resume and persistence.
extension WizardSession on Wizard {
  void _persistProgress(WizardPhase phase) {
    final client = _ref.read(kustaviClientProvider).value;
    if (client == null) {
      return;
    }
    final request = pb.SaveSessionStateRequest()
      ..step = phase.stepIndex
      ..qualityThresholds = (pb.RunQualityPassRequest()
        ..blurThreshold = _blurThreshold
        ..underexposedThreshold = _underexposedThreshold
        ..overexposedThreshold = _overexposedThreshold)
      ..tripParams = _tripsRequest();
    unawaited(_safeSave(client, request));
  }

  void _persistDecisions(DeletionIntent plan) {
    final client = _ref.read(kustaviClientProvider).value;
    if (client == null) {
      return;
    }
    final phase = _state.value;
    if (phase == null ||
        phase is WizardStart ||
        phase is WizardScanning ||
        phase is WizardSessionRestore) {
      return;
    }
    final request = pb.SaveSessionStateRequest()..replaceDecisions = true;
    for (final id in plan.explicitKept) {
      request.decisions.add(
        pb.DecisionEntry()
          ..imageId = id
          ..decision = pb.Decision.KEEP,
      );
    }
    for (final id in plan.explicitDeleted) {
      request.decisions.add(
        pb.DecisionEntry()
          ..imageId = id
          ..decision = pb.Decision.DELETE,
      );
    }
    plan.groupKeepers.forEach((groupId, keeperId) {
      request.groupKeepers[groupId] = keeperId;
    });
    unawaited(_safeSave(client, request));
  }

  void selectFolder(String folder) {
    if (_state.value is! WizardStart) {
      return;
    }
    _clearPassResults();
    _returnPhase = null;
    _resumeTargetStep = null;
    _resuming = false;
    _sourceFolder = folder;
    final client = _ref.read(kustaviClientProvider);
    if (client case AsyncData<KustaviClient>(:final value)) {
      unawaited(_beginFromFolder(value, folder));
    } else if (client case AsyncError(:final error, :final stackTrace)) {
      _state = AsyncValue.error(error, stackTrace);
    } else {
      _state = AsyncValue.error(
        BackendRpc(
          const GrpcError.unavailable(
            'Back end is still starting up. Please wait a moment.',
          ),
        ),
        StackTrace.current,
      );
    }
  }

  /// Probe the folder for saved progress before scanning: a resumable session
  /// routes to [WizardSessionRestore]; otherwise a fresh scan starts.
  Future<void> _beginFromFolder(KustaviClient client, String folder) async {
    pb.InspectSessionResponse probe;
    try {
      probe = await client.inspectSession(folder);
    } on Object {
      probe = pb.InspectSessionResponse(); // treat a probe failure as "fresh"
    }
    if (_state.value is! WizardStart) {
      return; // the user navigated away while the probe was in flight
    }
    if (probe.hasSession && probe.imageCount > 0) {
      _state = AsyncValue.data(
        WizardSessionRestore(
          folder: folder,
          imageCount: probe.imageCount,
          savedStepIndex: probe.resumeStep,
        ),
      );
    } else {
      _startFreshScan(client, folder);
    }
  }

  void _startFreshScan(KustaviClient client, String folder) {
    _state = AsyncValue.data(WizardScanning(folder: folder));
    final request = pb.ScanFolderRequest()
      ..folder = folder
      ..recursive = true
      ..resume = false;
    _subscribe(client.scanFolder(request), _onScanEvent, _onScanDone);
  }

  /// [WizardSessionRestore] "Resume": re-emit the saved index, then rehydrate
  /// results/decisions and re-enter the pipeline at the saved step.
  void resumeSession() {
    if (_state.value is! WizardSessionRestore) {
      return;
    }
    final folder = (_state.value as WizardSessionRestore).folder;
    _clearPassResults();
    _sourceFolder = folder;
    _resuming = true;
    _state = AsyncValue.data(WizardScanning(folder: folder));
    final client = _ref.read(kustaviClientProvider).requireValue;
    final request = pb.ScanFolderRequest()
      ..folder = folder
      ..recursive = true
      ..resume = true;
    _subscribe(client.scanFolder(request), _onScanEvent, _onResumeScanDone);
  }

  /// [WizardSessionRestore] "Start fresh": discard saved progress and scan.
  void startFreshFromRestore() {
    if (_state.value is! WizardSessionRestore) {
      return;
    }
    final folder = (_state.value as WizardSessionRestore).folder;
    _clearPassResults();
    _returnPhase = null;
    _resumeTargetStep = null;
    _resuming = false;
    _sourceFolder = folder;
    final client = _ref.read(kustaviClientProvider).requireValue;
    _startFreshScan(client, folder);
  }

  Future<void> _onResumeScanDone() async {
    if (_state.value is! WizardScanning) {
      return;
    }
    final complete = _pendingScanComplete;
    _pendingScanComplete = null;
    if (complete == null || complete.images == 0) {
      _resuming = false;
      _state = AsyncValue.data(
        WizardConfirmFolder(
          folder: _sourceFolder,
          imageCount: _orderedIds.length,
        ),
      );
      return;
    }

    final client = _ref.read(kustaviClientProvider).requireValue;
    pb.GetSessionResultsResponse results;
    try {
      results = await client.getSessionResults();
    } on Object catch (error, stackTrace) {
      _resuming = false;
      _state = AsyncValue.error(
        error is BackendError ? error : mapToBackendError(error),
        stackTrace,
      );
      return;
    }

    // Restore tunables (proto3 zeroes unset fields -> keep the defaults).
    final t = results.qualityThresholds;
    if (t.blurThreshold > 0) _blurThreshold = t.blurThreshold;
    if (t.underexposedThreshold > 0) {
      _underexposedThreshold = t.underexposedThreshold;
    }
    if (t.overexposedThreshold > 0) {
      _overexposedThreshold = t.overexposedThreshold;
    }
    final p = results.tripParams;
    if (p.maxGapHours > 0) _tripGapHours = p.maxGapHours;
    if (p.maxDistanceKm > 0) _tripDistanceKm = p.maxDistanceKm;
    if (p.homeRadiusKm > 0) _tripHomeRadiusKm = p.homeRadiusKm;
    if (p.legRadiusKm > 0) _tripLegRadiusKm = p.legRadiusKm;
    _saveLastRunThresholds();

    // Restore every persisted pass result. Nothing here re-runs; the ladder
    // below only runs a pass that never finished (or was never reached).
    _qualityFlags.clear();
    for (final flag in results.qualityFlags) {
      _qualityFlags[flag.imageId] = QualityFlagInfo.fromFlag(flag);
    }
    _similarGroups.clear();
    for (final group in results.similarGroups) {
      _similarGroups.add(SimilarGroupInfo.fromGroup(group));
    }
    _junkFlags.clear();
    for (final flag in results.junkFlags) {
      _junkFlags[flag.imageId] = JunkFlagInfo.fromFlag(flag);
    }
    _videoFlags.clear();
    for (final flag in results.videoFlags) {
      _videoFlags[flag.videoId] = VideoFlagInfo.fromFlag(flag);
    }
    _videoTotal = results.videoTotal;

    // Restore the user's keep/delete choices.
    final kept = <String>{};
    final deleted = <String>{};
    for (final entry in results.decisions) {
      if (entry.decision == pb.Decision.DELETE) {
        deleted.add(entry.imageId);
      } else if (entry.decision == pb.Decision.KEEP) {
        kept.add(entry.imageId);
      }
    }
    final keepers = <int, String>{};
    results.groupKeepers.forEach((groupId, keeperId) {
      keepers[groupId] = keeperId;
    });
    _hydratingDecisions = true;
    _ref
        .read(deletionPlanProvider.notifier)
        .hydrate(kept: kept, deleted: deleted, keepers: keepers);
    _hydratingDecisions = false;

    _resuming = false;

    final target = complete.resumeStep;
    if (target < WizardStep.quality.index) {
      _state = AsyncValue.data(
        WizardConfirmFolder(
          folder: _sourceFolder,
          imageCount: _orderedIds.length,
        ),
      );
      return;
    }

    // Pass results are restored above. Batches come from the trips pass,
    // which is cheap and has no persisted result, so it runs again; the
    // user then lands on the batch menu (or the commit summary).
    _resumeTargetStep = target;
    _sessionPassDone
      ..clear()
      ..addAll([
        if (results.qualityDone) WizardStep.quality.index,
        if (results.similarDone) WizardStep.duplicates.index,
        if (results.junkDone) WizardStep.junk.index,
        if (results.videoDone) WizardStep.video.index,
      ]);
    _batchPassDone
      ..clear()
      ..addAll(results.completedBatchPasses);
    _returnPhase = WizardConfirmFolder(
      folder: _sourceFolder,
      imageCount: _orderedIds.length,
    );
    _startTripsPass();
  }

  void cancelScan() {
    if (_state.value is! WizardScanning) {
      return;
    }
    _cancelPass();
    _state = const AsyncValue.data(WizardStart());
  }

  void _onScanEvent(pb.ScanEvent event) {
    final phase = _state.value;
    if (phase is! WizardScanning) {
      return;
    }
    switch (event.whichEvent()) {
      case pb.ScanEvent_Event.progress:
        _state = AsyncValue.data(
          phase.copyWith(
            filesSeen: event.progress.filesSeen,
            imagesFound: event.progress.imagesFound,
            currentPath: event.progress.currentPath,
          ),
        );
      case pb.ScanEvent_Event.image:
        final meta = event.image;
        if (!_images.containsKey(meta.id)) {
          _orderedIds.add(meta.id);
        }
        _images[meta.id] = ImageInfo.fromMeta(meta);
        _state = AsyncValue.data(
          phase.copyWith(
            imagesFound: _orderedIds.length,
            currentPath: meta.name,
          ),
        );
      case pb.ScanEvent_Event.complete:
        _pendingScanComplete = event.complete;
      case pb.ScanEvent_Event.notSet:
        break;
    }
  }

  void _onScanDone() {
    if (_state.value is! WizardScanning) {
      return;
    }
    final folder = (_state.value as WizardScanning).folder;
    final complete = _pendingScanComplete;
    _pendingScanComplete = null;
    if (complete == null) {
      return;
    }
    if (complete.images == 0) {
      _state = AsyncValue.data(WizardNoImages(folder: folder));
    } else {
      // Fresh scan: a saved session is detected earlier (in [selectFolder] via
      // InspectSession) and handled by [resumeSession] / [_onResumeScanDone].
      _state = AsyncValue.data(
        WizardConfirmFolder(
          folder: folder,
          imageCount: _orderedIds.length,
          scanErrors: complete.errors,
        ),
      );
    }
  }

  void backFromConfirm() {
    if (_state.value is! WizardConfirmFolder) {
      return;
    }
    _clearPassResults();
    _state = const AsyncValue.data(WizardStart());
  }

  /// Confirm -> Organize: the trips pass runs first, because its folders
  /// become the batches the user reviews.
  void continueFromConfirm() {
    if (_state.value is! WizardConfirmFolder) {
      return;
    }
    _returnPhase = _state.value;
    _startTripsPass();
  }

  /// [Back] on the step error screen: return to the phase the failed pass
  /// was started from.
  void goBackFromError() {
    if (_batchMode && _batches.isNotEmpty) {
      // A batch pass failed: keep every result so far and return to the menu.
      _cancelPass();
      _returnToBatchMenu();
      return;
    }
    _clearPassResults(keepReturnPhase: true);
    _state = AsyncValue.data(_returnPhase ?? const WizardStart());
  }

  /// Resets the wizard to S0 (S13 [Start over]); the next folder selection
  /// starts a new back-end session.
  void resetToStart() {
    _clearPassResults();
    _ref.read(deletionPlanProvider.notifier).reset();
    _state = const AsyncValue.data(WizardStart());
  }
}
