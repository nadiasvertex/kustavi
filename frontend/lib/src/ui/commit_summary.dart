import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../state/wizard.dart';
import 'format.dart';

/// S11 — commit summary: keep/left-behind counts and the destination picker
/// (spec/frontend.md §6.2). The [Copy] button lives in the shell action bar
/// and is enabled once [destination] is non-empty.
class CommitSummaryScreen extends ConsumerStatefulWidget {
  const CommitSummaryScreen({
    super.key,
    required this.keepCount,
    required this.keepBytes,
    required this.leftBehindCount,
    required this.destination,
    this.pickDirectory,
  });

  final int keepCount;
  final int keepBytes;
  final int leftBehindCount;

  /// Current destination value (the suggested `<source>-kept` sibling until
  /// the user edits it).
  final String destination;

  /// Directory-picker override for tests; defaults to `file_picker`.
  final Future<String?> Function()? pickDirectory;

  @override
  ConsumerState<CommitSummaryScreen> createState() =>
      _CommitSummaryScreenState();
}

class _CommitSummaryScreenState extends ConsumerState<CommitSummaryScreen> {
  late final TextEditingController _controller =
      TextEditingController(text: widget.destination);

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  void _onChanged(String value) {
    ref.read(wizardProvider.notifier).setCommitDestination(value);
  }

  Future<void> _choose() async {
    final picked = widget.pickDirectory != null
        ? await widget.pickDirectory!()
        : await FilePicker.platform.getDirectoryPath();
    if (picked == null || picked.isEmpty) {
      return;
    }
    _controller.text = picked;
    _onChanged(picked);
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Center(
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 560),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              'Keep ${formatInt(widget.keepCount)} photos '
              '(${formatBytes(widget.keepBytes)})',
              style: theme.textTheme.titleLarge,
            ),
            const SizedBox(height: 4),
            Text(
              widget.leftBehindCount == 1
                  ? '1 photo will be left behind'
                  : '${formatInt(widget.leftBehindCount)} photos will be left '
                      'behind',
              style: theme.textTheme.bodyMedium
                  ?.copyWith(color: theme.colorScheme.outline),
            ),
            const SizedBox(height: 24),
            Row(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Expanded(
                  child: TextField(
                    controller: _controller,
                    onChanged: _onChanged,
                    decoration: const InputDecoration(
                      labelText: 'Destination',
                      border: OutlineInputBorder(),
                      isDense: true,
                    ),
                  ),
                ),
                const SizedBox(width: 8),
                OutlinedButton(
                  onPressed: _choose,
                  child: const Text('Choose folder…'),
                ),
              ],
            ),
            const SizedBox(height: 8),
            Text(
              'The folder is created if it does not exist. Your original '
              'files are never modified.',
              style: theme.textTheme.bodySmall
                  ?.copyWith(color: theme.colorScheme.outline),
            ),
          ],
        ),
      ),
    );
  }
}
