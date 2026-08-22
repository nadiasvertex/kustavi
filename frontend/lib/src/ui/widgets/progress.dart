import 'package:flutter/material.dart';

import '../format.dart';

/// A running-pass screen: step title, progress bar, `done / total` count,
/// and optional expectation copy (spec/frontend.md §6.2 S3/S6/S8).
class PassProgressScreen extends StatelessWidget {
  const PassProgressScreen({
    super.key,
    required this.title,
    required this.done,
    required this.total,
    this.caption,
    this.currentFile,
  });

  final String title;
  final int done;
  final int total;

  /// Expectation-setting copy, e.g. the Moondream speed note.
  final String? caption;
  final String? currentFile;

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
            Text(title, style: theme.textTheme.titleLarge),
            const SizedBox(height: 20),
            LinearProgressIndicator(value: fraction),
            const SizedBox(height: 12),
            Text(
              total > 0
                  ? '${formatInt(done)} / ${formatInt(total)}'
                  : formatInt(done),
              style: theme.textTheme.bodyMedium,
            ),
            if (caption != null) ...[
              const SizedBox(height: 8),
              Text(
                caption!,
                style: theme.textTheme.bodySmall,
                textAlign: TextAlign.center,
              ),
            ],
            if (currentFile != null && currentFile!.isNotEmpty) ...[
              const SizedBox(height: 8),
              Text(
                currentFile!,
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
