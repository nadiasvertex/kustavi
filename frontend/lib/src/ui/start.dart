import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../state/model_status.dart';
import '../state/wizard.dart';
import 'format.dart';

/// S0 — the start screen (spec/frontend.md §6.2).
class StartScreen extends ConsumerWidget {
  const StartScreen({super.key, required this.onPickDirectory});

  /// Opens the directory picker; returns the selected path or null.
  final Future<String?> Function() onPickDirectory;

  Future<void> _selectFolder(WidgetRef ref) async {
    final folder = await onPickDirectory();
    if (folder == null || folder.isEmpty) {
      return;
    }
    ref.read(wizardProvider.notifier).selectFolder(folder);
  }

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final theme = Theme.of(context);
    final model = ref.watch(modelStatusProvider).value;
    return Center(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Text('Kustavi', style: theme.textTheme.displayMedium),
          const SizedBox(height: 8),
          Text(
            'Select a folder of photos to keep the best of.',
            style: theme.textTheme.bodyMedium
                ?.copyWith(color: theme.colorScheme.outline),
          ),
          const SizedBox(height: 24),
          FilledButton.icon(
            onPressed: () => _selectFolder(ref),
            icon: const Icon(Icons.folder_open),
            label: const Text('Select folder…'),
          ),
          if (model is ModelPrepUnknown || model is ModelPrepDownloading) ...[
            const SizedBox(height: 24),
            _ModelPrepCard(state: model),
          ],
        ],
      ),
    );
  }
}

/// "Preparing vision model — 42% (1.2 / 2.9 GB)" (§6.2, §6.3).
class _ModelPrepCard extends StatelessWidget {
  const _ModelPrepCard({required this.state});

  final ModelPrepState? state;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final fraction = switch (state) {
      ModelPrepDownloading(:final fraction) => fraction,
      _ => null,
    };
    final label = switch (state) {
      ModelPrepDownloading(
        :final doneBytes,
        :final totalBytes,
      ) when fraction != null =>
        'Preparing vision model — ${(fraction * 100).toInt()}% '
            '(${formatGb(doneBytes)} / ${formatGb(totalBytes)})',
      _ => 'Preparing vision model…',
    };
    return Card(
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 360),
          child: Column(
            children: [
              Text(
                label,
                style: theme.textTheme.bodyMedium,
                textAlign: TextAlign.center,
              ),
              const SizedBox(height: 8),
              LinearProgressIndicator(value: fraction),
            ],
          ),
        ),
      ),
    );
  }
}
