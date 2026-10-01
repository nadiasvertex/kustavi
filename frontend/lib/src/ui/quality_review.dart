import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../state/decisions.dart';
import '../state/wizard.dart';
import 'format.dart';
import 'review.dart';

/// S4 — quality review of back-end-flagged candidates
/// (spec/frontend.md §6.2).
class QualityReviewScreen extends ConsumerStatefulWidget {
  const QualityReviewScreen({
    super.key,
    required this.flaggedCount,
    required this.totalImages,
    required this.rerunEnabled,
    this.previewFlagged,
  });

  final int flaggedCount;
  final int totalImages;
  final bool rerunEnabled;

  /// Photos the current slider values would flag, or null while unknown.
  final int? previewFlagged;

  @override
  ConsumerState<QualityReviewScreen> createState() =>
      _QualityReviewScreenState();
}

class _QualityReviewScreenState extends ConsumerState<QualityReviewScreen> {
  /// Collapsed on entry so the flagged grid gets the full viewport; the
  /// common "nudge a slider and re-run" loop is reachable from the collapsed
  /// header.
  bool _thresholdsExpanded = false;

  @override
  Widget build(BuildContext context) {
    ref.watch(wizardProvider);
    final wizard = ref.read(wizardProvider.notifier);
    final theme = Theme.of(context);

    return Padding(
      padding: const EdgeInsets.only(bottom: 16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Flexible(
            fit: FlexFit.tight,
            child: FlaggedReview(
              step: DeletionStep.quality,
              header:
                  '${widget.flaggedCount} of '
                  '${formatInt(widget.totalImages)} images flagged',
            ),
          ),
          const Divider(height: 1),
          _thresholdPanel(theme, wizard),
        ],
      ),
    );
  }

  Widget _thresholdPanel(ThemeData theme, Wizard wizard) {
    final summary =
        'Blur ${_fmtBlur(wizard.blurThreshold)} · '
        'Under ${wizard.underexposedThreshold.toStringAsFixed(2)} · '
        'Over ${wizard.overexposedThreshold.toStringAsFixed(2)}';

    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        InkWell(
          onTap: () =>
              setState(() => _thresholdsExpanded = !_thresholdsExpanded),
          child: Padding(
            padding: const EdgeInsets.fromLTRB(16, 10, 8, 10),
            child: Row(
              children: [
                Icon(
                  _thresholdsExpanded ? Icons.expand_more : Icons.chevron_right,
                ),
                const SizedBox(width: 8),
                Text(
                  'Quality thresholds',
                  style: theme.textTheme.titleSmall?.copyWith(
                    fontWeight: FontWeight.w600,
                  ),
                ),
                const SizedBox(width: 12),
                if (!_thresholdsExpanded)
                  Expanded(
                    child: Text(
                      summary,
                      style: theme.textTheme.bodySmall?.copyWith(
                        color: theme.colorScheme.onSurfaceVariant,
                      ),
                      overflow: TextOverflow.ellipsis,
                    ),
                  )
                else
                  const Spacer(),
                if (widget.rerunEnabled)
                  IconButton(
                    tooltip: 'Rerun pass',
                    icon: const Icon(Icons.refresh),
                    onPressed: wizard.rerunQualityPass,
                  ),
              ],
            ),
          ),
        ),
        if (_thresholdsExpanded)
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Wrap(
                  spacing: 24,
                  runSpacing: 4,
                  children: [
                    SizedBox(
                      width: 260,
                      child: _slider(
                        theme,
                        label: 'Blur threshold',
                        value: wizard.blurThreshold,
                        valueLabel: _fmtBlur(wizard.blurThreshold),
                        lowHint: 'Fewer flagged',
                        highHint: 'More flagged',
                        min: 10,
                        max: 500,
                        divisions: 49,
                        onChanged: wizard.setBlurThreshold,
                      ),
                    ),
                    SizedBox(
                      width: 260,
                      child: _slider(
                        theme,
                        label: 'Underexposed',
                        value: wizard.underexposedThreshold,
                        valueLabel: wizard.underexposedThreshold
                            .toStringAsFixed(2),
                        lowHint: 'More flagged',
                        highHint: 'Fewer flagged',
                        min: 0,
                        max: 0.8,
                        divisions: 16,
                        onChanged: wizard.setUnderexposedThreshold,
                      ),
                    ),
                    SizedBox(
                      width: 260,
                      child: _slider(
                        theme,
                        label: 'Overexposed',
                        value: wizard.overexposedThreshold,
                        valueLabel: wizard.overexposedThreshold.toStringAsFixed(
                          2,
                        ),
                        lowHint: 'More flagged',
                        highHint: 'Fewer flagged',
                        min: 0,
                        max: 0.8,
                        divisions: 16,
                        onChanged: wizard.setOverexposedThreshold,
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 4),
                Row(
                  children: [
                    TextButton.icon(
                      onPressed: wizard.resetThresholds,
                      icon: const Icon(Icons.restore, size: 18),
                      label: const Text('Reset to defaults'),
                    ),
                    if (widget.rerunEnabled) ...[
                      const SizedBox(width: 8),
                      FilledButton.icon(
                        onPressed: wizard.rerunQualityPass,
                        icon: const Icon(Icons.refresh, size: 18),
                        label: const Text('Rerun pass'),
                      ),
                    ],
                    const SizedBox(width: 16),
                    Expanded(child: _previewLine(theme)),
                  ],
                ),
              ],
            ),
          ),
      ],
    );
  }

  /// Live count for the current slider values, shown before a rerun so the
  /// user can see which way a change moves the result.
  Widget _previewLine(ThemeData theme) {
    final preview = widget.previewFlagged;
    final style = theme.textTheme.bodyMedium?.copyWith(
      color: theme.colorScheme.onSurfaceVariant,
    );
    if (preview == null) {
      return Text(
        '${widget.flaggedCount} flagged at the last run',
        style: style,
      );
    }
    return Text(
      'These settings flag $preview of ${formatInt(widget.totalImages)} '
      'images (${widget.flaggedCount} at the last run)',
      style: style,
      overflow: TextOverflow.ellipsis,
    );
  }

  static String _fmtBlur(double value) =>
      value >= 100 && value == value.roundToDouble()
      ? value.toInt().toString()
      : value.toStringAsFixed(1);

  Widget _slider(
    ThemeData theme, {
    required String label,
    required double value,
    required String valueLabel,
    required String lowHint,
    required String highHint,
    required double min,
    required double max,
    required int divisions,
    required ValueChanged<double> onChanged,
  }) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          mainAxisAlignment: MainAxisAlignment.spaceBetween,
          children: [
            Text(label, style: theme.textTheme.bodyMedium),
            Text(
              valueLabel,
              style: theme.textTheme.bodyMedium?.copyWith(
                color: theme.colorScheme.primary,
                fontWeight: FontWeight.w600,
              ),
            ),
          ],
        ),
        Slider(
          value: value,
          min: min,
          max: max,
          divisions: divisions,
          label: valueLabel,
          onChanged: onChanged,
        ),
        Row(
          mainAxisAlignment: MainAxisAlignment.spaceBetween,
          children: [
            Flexible(
              child: Text(
                '← $lowHint',
                style: theme.textTheme.bodySmall,
                overflow: TextOverflow.ellipsis,
              ),
            ),
            Flexible(
              child: Text(
                '$highHint →',
                style: theme.textTheme.bodySmall,
                overflow: TextOverflow.ellipsis,
              ),
            ),
          ],
        ),
      ],
    );
  }
}
