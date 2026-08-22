import 'package:flutter/material.dart';

/// S1 exit: the folder contained no images (spec/frontend.md §6.2).
class NoImagesScreen extends StatelessWidget {
  const NoImagesScreen({super.key, required this.folder});

  final String folder;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Center(
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 32),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(
              Icons.folder_off,
              size: 48,
              color: theme.colorScheme.outline,
            ),
            const SizedBox(height: 16),
            Text(
              'No images found in $folder',
              style: theme.textTheme.titleLarge,
              textAlign: TextAlign.center,
            ),
            const SizedBox(height: 8),
            Text(
              'Choose another folder to continue.',
              style: theme.textTheme.bodyMedium,
            ),
          ],
        ),
      ),
    );
  }
}

/// A pass terminated in a non-cancel back-end error (§10.2).
class StepErrorScreen extends StatelessWidget {
  const StepErrorScreen({super.key, required this.message});

  final String message;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Center(
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 520),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(
              Icons.error_outline,
              size: 48,
              color: theme.colorScheme.error,
            ),
            const SizedBox(height: 16),
            Text('Processing error', style: theme.textTheme.titleLarge),
            const SizedBox(height: 8),
            Text(
              message,
              style: theme.textTheme.bodyMedium,
              textAlign: TextAlign.center,
            ),
          ],
        ),
      ),
    );
  }
}

/// Modal shown over whatever step is active when the back-end process dies
/// (§10.1). Returns 'retry' or 'exit'.
Future<String?> showCrashDialog(BuildContext context) {
  return showDialog<String>(
    context: context,
    builder: (context) => AlertDialog(
      title: const Text('Processing error'),
      content: const Text('The back end stopped unexpectedly.'),
      actions: [
        TextButton(
          onPressed: () => Navigator.of(context).pop('exit'),
          child: const Text('Exit app'),
        ),
        FilledButton(
          onPressed: () => Navigator.of(context).pop('retry'),
          child: const Text('Retry'),
        ),
      ],
    ),
  );
}
