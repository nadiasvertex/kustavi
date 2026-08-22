import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../backend/process.dart';
import '../state/model_status.dart';
import 'format.dart';

/// S5 — junk preparation: the vision-model download with byte progress,
/// speed, cancel, and the no-skip failure path (spec/frontend.md §6.2).
class JunkPrepScreen extends ConsumerWidget {
  const JunkPrepScreen({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final theme = Theme.of(context);
    final model = ref.watch(modelStatusProvider).value;
    return switch (model) {
      ModelPrepReady() => const SizedBox(),
      ModelPrepFailed(:final message) => Center(
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 520),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                Icon(
                  Icons.cloud_off,
                  size: 48,
                  color: theme.colorScheme.error,
                ),
                const SizedBox(height: 16),
                Text(
                  'The vision model download failed',
                  style: theme.textTheme.titleLarge,
                ),
                const SizedBox(height: 8),
                Text(
                  message,
                  style: theme.textTheme.bodyMedium,
                  textAlign: TextAlign.center,
                ),
                const SizedBox(height: 24),
                Row(
                  mainAxisAlignment: MainAxisAlignment.center,
                  children: [
                    OutlinedButton(
                      onPressed: () => ref
                          .read(modelStatusProvider.notifier)
                          .retryDownload(),
                      child: const Text('Retry download'),
                    ),
                    const SizedBox(width: 12),
                    TextButton(
                      onPressed: () => exitApp(ref),
                      child: const Text('Exit app'),
                    ),
                  ],
                ),
              ],
            ),
          ),
        ),
      ModelPrepDownloading(
            :final doneBytes,
            :final totalBytes,
            :final speedBps,
          ) =>
        _downloading(
          theme,
          doneBytes: doneBytes,
          totalBytes: totalBytes,
          speedBps: speedBps,
        ),
      ModelPrepUnknown() || null => _downloading(
          theme,
          doneBytes: 0,
          totalBytes: 0,
          speedBps: 0,
        ),
    };
  }

  Widget _downloading(
    ThemeData theme, {
    required int doneBytes,
    required int totalBytes,
    required double speedBps,
  }) {
    final fraction = totalBytes > 0 ? doneBytes / totalBytes : null;
    return Center(
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 520),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Text('Downloading vision model', style: theme.textTheme.titleLarge),
            const SizedBox(height: 20),
            LinearProgressIndicator(value: fraction),
            const SizedBox(height: 12),
            Text(
              fraction == null
                  ? formatBytes(doneBytes)
                  : '${formatGb(doneBytes)} / ${formatGb(totalBytes)} · '
                      '${(speedBps / 1e6).toStringAsFixed(1)} MB/s',
              style: theme.textTheme.bodyMedium,
            ),
            const SizedBox(height: 8),
            Text(
              'Moondream-3.1 powers the junk pass and is required; '
              'the download runs in the background.',
              style: theme.textTheme.bodySmall,
              textAlign: TextAlign.center,
            ),
          ],
        ),
      ),
    );
  }
}
