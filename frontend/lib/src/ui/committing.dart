import 'package:flutter/material.dart';

import 'format.dart';

/// S12 — commit in flight: file/byte progress and the current file name
/// (spec/frontend.md §6.2). [Cancel] lives in the shell action bar; partially
/// copied files are safe to leave (re-committing is idempotent).
class CommittingScreen extends StatelessWidget {
  const CommittingScreen({
    super.key,
    required this.done,
    required this.total,
    required this.doneBytes,
    required this.totalBytes,
    this.currentName = '',
  });

  final int done;
  final int total;
  final int doneBytes;
  final int totalBytes;
  final String currentName;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final fraction = total > 0 ? (done / total).clamp(0.0, 1.0) : null;
    return Center(
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 520),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Text('Copying photos', style: theme.textTheme.titleLarge),
            const SizedBox(height: 20),
            LinearProgressIndicator(value: fraction),
            const SizedBox(height: 12),
            Text(
              '${formatInt(done)} / ${formatInt(total)} files '
              '(${formatBytes(doneBytes)} / ${formatBytes(totalBytes)})',
              style: theme.textTheme.bodyMedium,
            ),
            if (currentName.isNotEmpty) ...[
              const SizedBox(height: 8),
              Text(
                currentName,
                style: theme.textTheme.bodySmall
                    ?.copyWith(fontFamily: 'monospace'),
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
              ),
            ],
          ],
        ),
      ),
    );
  }
}
