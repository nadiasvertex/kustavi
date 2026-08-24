import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../state/decisions.dart';
import '../state/wizard.dart';
import 'format.dart';
import 'review.dart';

/// S4 — quality review of back-end-flagged candidates
/// (spec/frontend.md §6.2).
class QualityReviewScreen extends ConsumerWidget {
  const QualityReviewScreen({
    super.key,
    required this.flaggedCount,
    required this.totalImages,
    required this.rerunEnabled,
  });

  final int flaggedCount;
  final int totalImages;
  final bool rerunEnabled;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
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
              header: '$flaggedCount of ${formatInt(totalImages)} images flagged',
            ),
          ),
          const Divider(height: 1),
          Padding(
            padding: const EdgeInsets.all(16),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  'Quality thresholds',
                  style: theme.textTheme.titleSmall?.copyWith(
                    fontWeight: FontWeight.w600,
                  ),
                ),
                const SizedBox(height: 8),
                _blurSlider(theme, wizard),
                const SizedBox(height: 12),
                _exposureSliders(theme, wizard),
                if (rerunEnabled) ...[
                  const SizedBox(height: 16),
                  FilledButton.icon(
                    onPressed: wizard.rerunQualityPass,
                    icon: const Icon(Icons.refresh, size: 18),
                    label: const Text('Rerun pass'),
                  ),
                ],
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _blurSlider(ThemeData theme, Wizard wizard) {
    final value = wizard.blurThreshold;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          mainAxisAlignment: MainAxisAlignment.spaceBetween,
          children: [
            Text(
              'Blur threshold',
              style: theme.textTheme.bodyMedium,
            ),
            Text(
              value >= 100 && value == value.roundToDouble()
                  ? value.toInt().toString()
                  : value.toStringAsFixed(1),
              style: theme.textTheme.bodyMedium?.copyWith(
                color: theme.colorScheme.primary,
                fontWeight: FontWeight.w600,
              ),
            ),
          ],
        ),
        Slider(
          value: value,
          min: 10,
          max: 500,
          divisions: 49,
          label: value.toStringAsFixed(0),
          onChanged: (v) => wizard.setBlurThreshold(v),
        ),
      ],
    );
  }

  Widget _exposureSliders(ThemeData theme, Wizard wizard) {
    return Row(
      children: [
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                mainAxisAlignment: MainAxisAlignment.spaceBetween,
                children: [
                  Text(
                    'Underexposed',
                    style: theme.textTheme.bodyMedium,
                  ),
                  Text(
                    wizard.underexposedThreshold.toStringAsFixed(2),
                    style: theme.textTheme.bodyMedium?.copyWith(
                      color: theme.colorScheme.primary,
                      fontWeight: FontWeight.w600,
                    ),
                  ),
                ],
              ),
              Slider(
                value: wizard.underexposedThreshold,
                min: 0,
                max: 0.8,
                divisions: 16,
                label: wizard.underexposedThreshold.toStringAsFixed(2),
                onChanged: (v) => wizard.setUnderexposedThreshold(v),
              ),
            ],
          ),
        ),
        const SizedBox(width: 16),
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                mainAxisAlignment: MainAxisAlignment.spaceBetween,
                children: [
                  Text(
                    'Overexposed',
                    style: theme.textTheme.bodyMedium,
                  ),
                  Text(
                    wizard.overexposedThreshold.toStringAsFixed(2),
                    style: theme.textTheme.bodyMedium?.copyWith(
                      color: theme.colorScheme.primary,
                      fontWeight: FontWeight.w600,
                    ),
                  ),
                ],
              ),
              Slider(
                value: wizard.overexposedThreshold,
                min: 0,
                max: 0.8,
                divisions: 16,
                label: wizard.overexposedThreshold.toStringAsFixed(2),
                onChanged: (v) => wizard.setOverexposedThreshold(v),
              ),
            ],
          ),
        ),
      ],
    );
  }
}
