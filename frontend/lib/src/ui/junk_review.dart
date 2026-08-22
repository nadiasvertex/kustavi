import 'package:flutter/material.dart';

import '../state/decisions.dart';
import 'format.dart';
import 'review.dart';

/// S7 — junk review of Moondream-flagged candidates
/// (spec/frontend.md §6.2).
class JunkReviewScreen extends StatelessWidget {
  const JunkReviewScreen({
    super.key,
    required this.flaggedCount,
    required this.totalImages,
  });

  final int flaggedCount;
  final int totalImages;

  @override
  Widget build(BuildContext context) {
    return FlaggedReview(
      step: DeletionStep.junk,
      header: '$flaggedCount of ${formatInt(totalImages)} images flagged',
    );
  }
}
