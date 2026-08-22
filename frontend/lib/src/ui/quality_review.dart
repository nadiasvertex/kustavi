import 'package:flutter/material.dart';

import '../state/decisions.dart';
import 'format.dart';
import 'review.dart';

/// S4 — quality review of back-end-flagged candidates
/// (spec/frontend.md §6.2).
class QualityReviewScreen extends StatelessWidget {
  const QualityReviewScreen({
    super.key,
    required this.flaggedCount,
    required this.totalImages,
  });

  final int flaggedCount;
  final int totalImages;

  @override
  Widget build(BuildContext context) {
    return FlaggedReview(
      step: DeletionStep.quality,
      header: '$flaggedCount of ${formatInt(totalImages)} images flagged',
    );
  }
}
