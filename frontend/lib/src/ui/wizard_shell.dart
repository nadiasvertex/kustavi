import 'dart:async';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../backend/process.dart';
import '../state/domain.dart';
import '../state/model_status.dart';
import '../state/phases.dart';
import '../state/wizard.dart';
import 'confirm_folder.dart';
import 'errors.dart';
import 'junk_prep.dart';
import 'junk_review.dart';
import 'placeholder.dart';
import 'quality_review.dart';
import 'scanning.dart';
import 'similar_review.dart';
import 'start.dart';
import 'trips_review.dart';
import 'widgets/progress.dart';

/// The wizard frame: step indicator, phase body, per-step action bar, and
/// the crash dialog (spec/frontend.md §6, §10.1).
class WizardShell extends ConsumerStatefulWidget {
  const WizardShell({super.key, this.pickDirectory});

  /// Directory-picker override for tests; defaults to `file_picker`.
  final Future<String?> Function()? pickDirectory;

  @override
  ConsumerState<WizardShell> createState() => _WizardShellState();
}

class _WizardShellState extends ConsumerState<WizardShell> {
  Future<String?> _pickDirectory() =>
      widget.pickDirectory != null
          ? widget.pickDirectory!()
          : FilePicker.platform.getDirectoryPath();

  void _retryAfterCrash() {
    ref.invalidate(backendProcessProvider);
    ref.read(wizardProvider.notifier).goBackFromError();
  }

  Future<void> _showCrashDialog() async {
    if (!mounted) {
      return;
    }
    final choice = await showCrashDialog(context);
    if (!mounted) {
      return;
    }
    if (choice == 'retry') {
      _retryAfterCrash();
    } else if (choice == 'exit') {
      await exitApp(ref);
    }
  }

  @override
  void initState() {
    super.initState();
    // Start the vision-model pipeline on app start (§6.3).
    ref.read(modelStatusProvider);
  }

  @override
  Widget build(BuildContext context) {
    // ref.listen is only legal inside build (riverpod 3); it re-registers on
    // every rebuild with the same key.
    ref.listen<AsyncValue<BackendEndpoint>>(
      backendProcessProvider,
      (previous, next) {
        if (next case AsyncError(:final error)) {
          if (error is BackendCrashed) {
            unawaited(_showCrashDialog());
          }
        }
      },
    );
    final phaseAsync = ref.watch(wizardProvider);
    final phase = phaseAsync.value;
    return Scaffold(
      appBar: AppBar(
        title: StepIndicator(currentIndex: phase?.stepIndex ?? 0),
        centerTitle: false,
      ),
      body: switch (phaseAsync) {
        AsyncData(:final value) => _body(context, value),
        AsyncLoading() => const Center(child: CircularProgressIndicator()),
        AsyncError(:final error) => StepErrorScreen(
            message: error is BackendError
                ? error.message
                : error.toString()),
      },
      bottomNavigationBar: phaseAsync is AsyncError
          ? _errorBar()
          : _actionBar(phase),
    );
  }

  Widget _body(BuildContext context, WizardPhase phase) => switch (phase) {
        WizardStart() => StartScreen(onPickDirectory: _pickDirectory),
        WizardScanning(
              :final folder,
              :final imagesFound,
              :final currentPath,
            ) =>
          ScanningScreen(
            folder: folder,
            imagesFound: imagesFound,
            currentPath: currentPath,
          ),
        WizardNoImages(:final folder) => NoImagesScreen(folder: folder),
        WizardConfirmFolder(
              :final folder,
              :final imageCount,
              :final scanErrors,
            ) =>
          ConfirmFolderScreen(
            folder: folder,
            imageCount: imageCount,
            scanErrors: scanErrors,
          ),
        WizardQualityRunning(:final done, :final total) =>
          PassProgressScreen(
            title: 'Checking image quality',
            done: done,
            total: total,
          ),
        WizardQualityReview(:final flaggedCount, :final totalImages, :final rerunEnabled) =>
          QualityReviewScreen(
            flaggedCount: flaggedCount,
            totalImages: totalImages,
            rerunEnabled: rerunEnabled,
          ),
        WizardJunkPrep() => const JunkPrepScreen(),
        WizardJunkRunning(:final done, :final total) => PassProgressScreen(
            title: 'Classifying with Moondream',
            done: done,
            total: total,
            caption:
                'Moondream takes about 1.5–3 seconds per image — expect '
                'this pass to be slow.',
          ),
        WizardJunkReview(:final flaggedCount, :final totalImages) =>
          JunkReviewScreen(
            flaggedCount: flaggedCount,
            totalImages: totalImages,
          ),
        WizardSimilarRunning(:final done, :final total) =>
          PassProgressScreen(
            title: 'Finding similar photos',
            done: done,
            total: total,
          ),
        WizardSessionRestore() =>
          const PlaceholderScreen('Saved session restore'),
        WizardSimilarReview(:final groupCount, :final markedCount) =>
          SimilarReviewScreen(
            groupCount: groupCount,
            markedCount: markedCount,
          ),
        WizardTripsRunning(:final done, :final total) =>
          PassProgressScreen(
            title: 'Grouping by trip',
            done: done,
            total: total,
          ),
        WizardTripsReview(:final tripCount, :final markedCount, :final trips, :final tripFolders) =>
          TripsReviewScreen(
            tripCount: tripCount,
            markedCount: markedCount,
            trips: trips,
            tripFolders: tripFolders,
          ),
        WizardCommitSummary() => const PlaceholderScreen('Commit summary'),
        WizardCommitting() => const PlaceholderScreen('Copying photos'),
        WizardDone() => const PlaceholderScreen('Done'),
      };

  Widget? _actionBar(WizardPhase? phase) {
    final wizard = ref.read(wizardProvider.notifier);
    final actions = switch (phase) {
      WizardStart() || WizardSessionRestore() => null,
      WizardScanning() => [
          OutlinedButton(
            onPressed: wizard.cancelScan,
            child: const Text('Cancel'),
          ),
        ],
      WizardNoImages() => [
          FilledButton(
            onPressed: wizard.resetToStart,
            child: const Text('Choose another folder'),
          ),
          TextButton(
            onPressed: () => exitApp(ref),
            child: const Text('Exit app'),
          ),
        ],
      WizardConfirmFolder() => [
          OutlinedButton(
            onPressed: wizard.backFromConfirm,
            child: const Text('Back'),
          ),
          FilledButton(
            onPressed: wizard.continueFromConfirm,
            child: const Text('Continue'),
          ),
        ],
      WizardQualityRunning() => [
          OutlinedButton(
            onPressed: wizard.cancelQuality,
            child: const Text('Cancel'),
          ),
        ],
      WizardQualityReview(:final flaggedCount) => [
          if (flaggedCount > 0) ...[
            OutlinedButton(
              onPressed: wizard.keepAllQualityFlagged,
              child: const Text('Keep all'),
            ),
            OutlinedButton(
              onPressed: wizard.markAllQualityFlagged,
              child: const Text('Mark all'),
            ),
            OutlinedButton(
              onPressed: wizard.backFromQuality,
              child: const Text('Back'),
            ),
          ],
          FilledButton(
            onPressed: wizard.continueFromQuality,
            child: const Text('Continue'),
          ),
        ],
      WizardJunkPrep() => [
          OutlinedButton(
            onPressed: wizard.cancelJunkPrep,
            child: const Text('Cancel'),
          ),
        ],
      WizardJunkRunning() => [
          OutlinedButton(
            onPressed: wizard.cancelJunk,
            child: const Text('Cancel'),
          ),
        ],
      WizardJunkReview(:final flaggedCount) => [
          if (flaggedCount > 0) ...[
            OutlinedButton(
              onPressed: wizard.keepAllJunkFlagged,
              child: const Text('Keep all'),
            ),
            OutlinedButton(
              onPressed: wizard.markAllJunkFlagged,
              child: const Text('Mark all'),
            ),
            OutlinedButton(
              onPressed: wizard.backFromJunk,
              child: const Text('Back'),
            ),
          ],
          FilledButton(
            onPressed: wizard.continueFromJunk,
            child: const Text('Continue'),
          ),
        ],
      WizardSimilarRunning() => [
          OutlinedButton(
            onPressed: wizard.cancelSimilar,
            child: const Text('Cancel'),
          ),
        ],
      WizardSimilarReview() => [
          OutlinedButton(
            onPressed: wizard.backFromSimilar,
            child: const Text('Back'),
          ),
          FilledButton(
            onPressed: wizard.continueFromSimilar,
            child: const Text('Continue'),
          ),
        ],
      WizardTripsRunning() => [
          OutlinedButton(
            onPressed: wizard.cancelTrips,
            child: const Text('Back'),
          ),
        ],
      WizardTripsReview() => [
          OutlinedButton(
            onPressed: wizard.backFromTrips,
            child: const Text('Back'),
          ),
          FilledButton(
            onPressed: wizard.continueFromTrips,
            child: const Text('Continue'),
          ),
        ],
      _ => null,
    };
    if (actions == null) {
      return null;
    }
    return BottomAppBar(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.end,
        children: [
          for (var i = 0; i < actions.length; i++) ...[
            if (i > 0) const SizedBox(width: 8),
            actions[i],
          ],
        ],
      ),
    );
  }

  /// [Back] / [Exit app] under a failed pass (§10.2).
  Widget _errorBar() {
    final wizard = ref.read(wizardProvider.notifier);
    return BottomAppBar(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.end,
        children: [
          OutlinedButton(
            onPressed: wizard.goBackFromError,
            child: const Text('Back'),
          ),
          const SizedBox(width: 8),
          TextButton(
            onPressed: () => exitApp(ref),
            child: const Text('Exit app'),
          ),
        ],
      ),
    );
  }
}

/// The six-step indicator in the app bar (spec/frontend.md §6.1):
/// completed steps checked, the current step highlighted.
class StepIndicator extends StatelessWidget {
  const StepIndicator({super.key, required this.currentIndex});

  /// 0-based current step; 6 = every step completed.
  final int currentIndex;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final items = <Widget>[];
    for (final step in WizardStep.values) {
      if (step.index > 0) {
        items.add(const SizedBox(width: 4));
        items.add(
          Icon(
            Icons.chevron_right,
            size: 14,
            color: theme.colorScheme.outline,
          ),
        );
        items.add(const SizedBox(width: 4));
      }
      final completed = step.index < currentIndex;
      final current = step.index == currentIndex;
      items.add(
        Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(
              completed
                  ? Icons.check_circle
                  : current
                      ? Icons.radio_button_checked
                      : Icons.radio_button_off,
              size: 16,
              color:
                  completed || current
                      ? theme.colorScheme.primary
                      : theme.colorScheme.outline,
            ),
            const SizedBox(width: 4),
            Text(
              step.label,
              style: theme.textTheme.bodySmall?.copyWith(
                color: current
                    ? theme.colorScheme.onSurface
                    : theme.colorScheme.outline,
                fontWeight: current ? FontWeight.w600 : null,
              ),
            ),
          ],
        ),
      );
    }
    return Row(children: items);
  }
}
