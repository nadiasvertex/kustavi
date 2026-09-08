import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../state/phases.dart';
import '../state/wizard.dart';

/// S0-B — a saved session was found in the picked folder; the user chooses to
/// resume where they left off or discard it and start over.
class SessionRestoreScreen extends ConsumerWidget {
  const SessionRestoreScreen({
    super.key,
    required this.folder,
    required this.imageCount,
    required this.savedStepIndex,
  });

  final String folder;
  final int imageCount;
  final int savedStepIndex;

  String get _stepLabel {
    if (savedStepIndex >= 0 && savedStepIndex < WizardStep.values.length) {
      return WizardStep.values[savedStepIndex].label;
    }
    return WizardStep.select.label;
  }

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final theme = Theme.of(context);
    final wizard = ref.read(wizardProvider.notifier);
    return Center(
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 520),
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 32),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              Icon(Icons.history, size: 48, color: theme.colorScheme.primary),
              const SizedBox(height: 16),
              Text(
                'Resume this session?',
                style: theme.textTheme.titleLarge,
                textAlign: TextAlign.center,
              ),
              const SizedBox(height: 8),
              Text(
                'Found saved progress in $folder — '
                '$imageCount images, stopped at the "$_stepLabel" step.',
                style: theme.textTheme.bodyMedium,
                textAlign: TextAlign.center,
              ),
              const SizedBox(height: 24),
              Wrap(
                spacing: 12,
                runSpacing: 12,
                alignment: WrapAlignment.center,
                children: [
                  OutlinedButton.icon(
                    onPressed: wizard.startFreshFromRestore,
                    icon: const Icon(Icons.refresh),
                    label: const Text('Start fresh'),
                  ),
                  FilledButton.icon(
                    onPressed: wizard.resumeSession,
                    icon: const Icon(Icons.play_arrow),
                    label: const Text('Resume'),
                  ),
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }
}
