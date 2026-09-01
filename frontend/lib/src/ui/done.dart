import 'package:flutter/material.dart';

import 'format.dart';

/// S13 — done: the copy result, plus any skips/errors (spec/frontend.md §6.2).
/// [Start over] / [Done] live in the shell action bar.
class DoneScreen extends StatelessWidget {
  const DoneScreen({
    super.key,
    required this.copiedCount,
    required this.destination,
    this.skippedCount = 0,
    this.errors = const <String>[],
  });

  final int copiedCount;
  final String destination;
  final int skippedCount;
  final List<String> errors;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final issues = <String>[
      if (skippedCount > 0)
        '$skippedCount ${skippedCount == 1 ? 'file was' : 'files were'} '
            'skipped (a different file of the same name already exists).',
      ...errors,
    ];
    return Center(
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 560),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(Icons.check_circle,
                    color: theme.colorScheme.primary, size: 28),
                const SizedBox(width: 8),
                Expanded(
                  child: Text(
                    'Copied ${formatInt(copiedCount)} '
                    '${copiedCount == 1 ? 'photo' : 'photos'} to $destination',
                    style: theme.textTheme.titleLarge,
                  ),
                ),
              ],
            ),
            const SizedBox(height: 12),
            Text(
              'Your original files were not modified. You can safely delete '
              'the source folder.',
              style: theme.textTheme.bodyMedium,
            ),
            if (issues.isNotEmpty) ...[
              const SizedBox(height: 20),
              Text(
                'Some files need your attention:',
                style: theme.textTheme.titleSmall
                    ?.copyWith(color: theme.colorScheme.error),
              ),
              const SizedBox(height: 8),
              ConstrainedBox(
                constraints: const BoxConstraints(maxHeight: 220),
                child: SingleChildScrollView(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      for (final issue in issues)
                        Padding(
                          padding: const EdgeInsets.only(bottom: 4),
                          child: Text(
                            issue,
                            style: theme.textTheme.bodySmall
                                ?.copyWith(fontFamily: 'monospace'),
                          ),
                        ),
                    ],
                  ),
                ),
              ),
            ],
          ],
        ),
      ),
    );
  }
}
