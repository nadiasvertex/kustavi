import 'package:flutter/material.dart';

import '../state/decisions.dart';
import 'format.dart';
import 'review.dart';

/// S10-C — review of videos flagged by the video pass (too short, corrupt,
/// blurry, static, or non-photographic content).
class VideoReviewScreen extends StatelessWidget {
  const VideoReviewScreen({
    super.key,
    required this.flaggedCount,
    required this.totalVideos,
  });

  final int flaggedCount;
  final int totalVideos;

  @override
  Widget build(BuildContext context) {
    return FlaggedReview(
      step: DeletionStep.video,
      header: '$flaggedCount of ${formatInt(totalVideos)} videos flagged',
    );
  }
}
