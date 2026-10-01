import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../state/domain.dart';
import '../state/phases.dart';
import '../state/wizard.dart';
import 'format.dart';

/// The batch menu (spec/frontend.md §6.2): pick a batch, then run whichever
/// passes you want on it, in any order. Reviews open from here and return
/// here, so nothing is forced into a fixed sequence.
class BatchMenuScreen extends ConsumerWidget {
  const BatchMenuScreen({
    super.key,
    required this.batches,
    required this.selectedKey,
    required this.markedCount,
    required this.totalImages,
  });

  final List<BatchSummary> batches;
  final String? selectedKey;
  final int markedCount;
  final int totalImages;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final theme = Theme.of(context);
    final wizard = ref.read(wizardProvider.notifier);
    BatchSummary? selected;
    for (final batch in batches) {
      if (batch.key == selectedKey) {
        selected = batch;
      }
    }

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 8, 16, 4),
          child: Text(
            '${batches.length} batches · '
            '${formatInt(markedCount)} of ${formatInt(totalImages)} photos '
            'marked for deletion',
            style: theme.textTheme.titleLarge,
          ),
        ),
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
          child: Text(
            'Run any passes on a batch, in any order. The fast passes '
            '(quality, duplicates) are worth running first: the slow junk '
            'pass skips photos already marked for deletion.',
            style: theme.textTheme.bodyMedium?.copyWith(
              color: theme.colorScheme.onSurfaceVariant,
            ),
          ),
        ),
        const Divider(height: 1),
        Expanded(
          child: Row(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              SizedBox(
                width: 320,
                child: ListView.builder(
                  itemCount: batches.length,
                  itemBuilder: (context, index) {
                    final batch = batches[index];
                    return _BatchTile(
                      batch: batch,
                      selected: batch.key == selectedKey,
                      onTap: () => wizard.selectBatch(batch.key),
                    );
                  },
                ),
              ),
              const VerticalDivider(width: 1),
              Expanded(
                child: selected == null
                    ? const Center(child: Text('No photos to review'))
                    : _BatchDetail(batch: selected, wizard: wizard),
              ),
            ],
          ),
        ),
      ],
    );
  }
}

/// The passes in their suggested order, with how costly each is.
const _passInfo = <(WizardStep, String, String)>[
  (WizardStep.quality, 'Quality', 'Blurry and badly exposed photos · fast'),
  (WizardStep.duplicates, 'Duplicates', 'Near-identical photos · fast'),
  (
    WizardStep.junk,
    'Junk',
    'Screenshots, scans and memes · slow, about 2 s per photo',
  ),
  (WizardStep.video, 'Video', 'Short, corrupt, blurry or static clips'),
];

class _BatchTile extends StatelessWidget {
  const _BatchTile({
    required this.batch,
    required this.selected,
    required this.onTap,
  });

  final BatchSummary batch;
  final bool selected;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final doneCount = batch.passes.values.where((s) => s.done).length;
    final video = batch.videoCount > 0
        ? ' · ${formatInt(batch.videoCount)} videos'
        : '';
    return ListTile(
      selected: selected,
      selectedTileColor: theme.colorScheme.secondaryContainer.withValues(
        alpha: 0.5,
      ),
      title: Text(batch.title, maxLines: 1, overflow: TextOverflow.ellipsis),
      subtitle: Text(
        '${formatInt(batch.photoCount)} photos$video',
        maxLines: 1,
        overflow: TextOverflow.ellipsis,
      ),
      trailing: doneCount == 0
          ? null
          : Text(
              '$doneCount done',
              style: theme.textTheme.labelMedium?.copyWith(
                color: theme.colorScheme.primary,
              ),
            ),
      onTap: onTap,
    );
  }
}

class _BatchDetail extends StatelessWidget {
  const _BatchDetail({required this.batch, required this.wizard});

  final BatchSummary batch;
  final Wizard wizard;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        Text(batch.title, style: theme.textTheme.titleMedium),
        const SizedBox(height: 12),
        for (final (step, label, hint) in _passInfo)
          // A batch with no videos has nothing for the video pass to do.
          if (step != WizardStep.video || batch.videoCount > 0)
            _PassCard(
              step: step,
              label: label,
              hint: hint,
              status:
                  batch.passes[step.index] ??
                  const BatchPassStatus(done: false),
              wizard: wizard,
            ),
      ],
    );
  }
}

class _PassCard extends StatelessWidget {
  const _PassCard({
    required this.step,
    required this.label,
    required this.hint,
    required this.status,
    required this.wizard,
  });

  final WizardStep step;
  final String label;
  final String hint;
  final BatchPassStatus status;
  final Wizard wizard;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final unit = step == WizardStep.duplicates ? 'groups' : 'flagged';
    final summary = status.done
        ? 'Done · ${status.flagged} $unit'
        : status.flagged > 0
        ? 'Partly run · ${status.flagged} $unit'
        : 'Not run';
    return Card(
      margin: const EdgeInsets.only(bottom: 12),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Row(
          children: [
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(label, style: theme.textTheme.titleSmall),
                  const SizedBox(height: 2),
                  Text(
                    hint,
                    style: theme.textTheme.bodySmall?.copyWith(
                      color: theme.colorScheme.onSurfaceVariant,
                    ),
                  ),
                  const SizedBox(height: 6),
                  Text(
                    summary,
                    style: theme.textTheme.bodyMedium?.copyWith(
                      color: status.done ? theme.colorScheme.primary : null,
                      fontWeight: status.done ? FontWeight.w600 : null,
                    ),
                  ),
                ],
              ),
            ),
            const SizedBox(width: 12),
            if (status.done) ...[
              OutlinedButton(
                onPressed: () => wizard.reviewBatchPass(step),
                child: const Text('Review'),
              ),
              const SizedBox(width: 8),
              TextButton(
                onPressed: () => wizard.startBatchPass(step),
                child: const Text('Run again'),
              ),
            ] else ...[
              FilledButton(
                onPressed: () => wizard.startBatchPass(step),
                child: Text(status.flagged > 0 ? 'Finish' : 'Run'),
              ),
              const SizedBox(width: 8),
              TextButton(
                onPressed: () => wizard.startPassOnAllBatches(step),
                child: const Text('Run on all batches'),
              ),
            ],
          ],
        ),
      ),
    );
  }
}
