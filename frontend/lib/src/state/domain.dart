import 'package:grpc/grpc.dart';

import '../generated/kustavi/service.pb.dart' as pb;

/// Immutable metadata for one scanned image.
///
/// Built incrementally from `ScanEvent`s and held by the wizard controller;
/// it is the single source of truth for image metadata in the GUI.
class ImageInfo {
  const ImageInfo({
    required this.id,
    required this.path,
    required this.name,
    required this.sizeBytes,
    required this.width,
    required this.height,
    this.taken,
    this.gps,
    required this.workingImagePath,
    this.isVideo = false,
    this.videoDuration,
  });

  /// Path relative to the session folder, e.g. `2024/paris/IMG_0001.jpg`.
  final String id;

  /// Absolute path to the original file.
  final String path;

  /// File name.
  final String name;

  final int sizeBytes;
  final int width;
  final int height;

  /// Capture date, when EXIF provided one.
  final DateTime? taken;

  /// WGS84 decimal degrees `(latitude, longitude)`, when present.
  final (double, double)? gps;

  /// Absolute path to the cached 768px working preview written by the back
  /// end before the `ImageMeta` event was emitted.
  final String workingImagePath;

  /// True when this is a video rather than a photo.
  final bool isVideo;

  /// Video length; null for photos or when the back end couldn't probe it.
  final Duration? videoDuration;

  factory ImageInfo.fromMeta(pb.ImageMeta meta) {
    return ImageInfo(
      id: meta.id,
      path: meta.path,
      name: meta.name,
      sizeBytes: meta.sizeBytes.toInt(),
      width: meta.width,
      height: meta.height,
      taken: meta.hasTakenUnixMs()
          ? DateTime.fromMillisecondsSinceEpoch(meta.takenUnixMs.toInt())
          : null,
      gps: meta.hasGps()
          ? (meta.gps.latitude, meta.gps.longitude)
          : null,
      workingImagePath: meta.thumbnailPath,
      isVideo: meta.kind == pb.MediaKind.VIDEO,
      videoDuration: meta.hasDurationMs()
          ? Duration(milliseconds: meta.durationMs.toInt())
          : null,
    );
  }
}

/// Human-facing quality reasons for a flagged image.
enum QualityReasonTag {
  blurry,
  underExposed,
  overExposed;

  String get label => switch (this) {
        blurry => 'Blurry',
        underExposed => 'Underexposed',
        overExposed => 'Overexposed',
      };

  static QualityReasonTag fromProto(int value) => switch (value) {
        1 => blurry,
        2 => underExposed,
        3 => overExposed,
        _ => throw ArgumentError('unrecognized quality reason: $value'),
      };
}

/// Blur / exposure verdict for one image, emitted only when flagged.
class QualityFlagInfo {
  const QualityFlagInfo({
    required this.imageId,
    required this.reasons,
    required this.sharpness,
    required this.exposureScore,
  });

  factory QualityFlagInfo.fromFlag(pb.QualityFlag flag) {
    return QualityFlagInfo(
      imageId: flag.imageId,
      reasons: flag.reasons.map((r) => QualityReasonTag.fromProto(r.value)).toList(),
      sharpness: flag.sharpness,
      exposureScore: flag.exposureScore,
    );
  }

  final String imageId;
  final List<QualityReasonTag> reasons;

  /// Laplacian variance; higher = sharper.
  final double sharpness;

  /// 0..1; 0.5 = ideal.
  final double exposureScore;
}

/// Moondream junk verdict for one image, emitted only when classified as a
/// non-photograph.
class JunkFlagInfo {
  const JunkFlagInfo({
    required this.imageId,
    required this.reason,
    required this.confidence,
  });

  factory JunkFlagInfo.fromFlag(pb.JunkFlag flag) {
    return JunkFlagInfo(
      imageId: flag.imageId,
      reason: flag.reason,
      confidence: flag.confidence,
    );
  }

  final String imageId;

  /// Short classification, e.g. "screenshot", "scan", "meme".
  final String reason;

  /// 0..1.
  final double confidence;
}

/// Video-pass verdict for one video, emitted only when flagged: too short,
/// corrupt/unplayable, blurry, static (no motion), or a junk category reused
/// from the vision classifier.
class VideoFlagInfo {
  const VideoFlagInfo({
    required this.videoId,
    required this.reason,
    required this.confidence,
  });

  factory VideoFlagInfo.fromFlag(pb.VideoFlag flag) {
    return VideoFlagInfo(
      videoId: flag.videoId,
      reason: flag.reason,
      confidence: flag.confidence,
    );
  }

  final String videoId;

  /// "too_short" | "corrupt" | "blurry" | "static" | a junk category.
  final String reason;

  /// 0..1.
  final double confidence;
}

/// A near-duplicate group, members best-first.
class SimilarGroupInfo {
  const SimilarGroupInfo({
    required this.id,
    required this.memberIds,
    required this.recommendedKeepId,
    required this.memberScores,
  });

  factory SimilarGroupInfo.fromGroup(pb.SimilarGroup group) {
    return SimilarGroupInfo(
      id: group.id,
      memberIds: List<String>.unmodifiable(group.imageIds),
      recommendedKeepId: group.recommendedKeepId,
      memberScores: List<double>.unmodifiable(group.memberScores),
    );
  }

  final int id;
  final List<String> memberIds;
  final String recommendedKeepId;

  /// Composite "bestness" scores, parallel to [memberIds].
  final List<double> memberScores;
}

/// A contiguous stay at one place within a trip (Rome, then Florence).
class TripLegInfo {
  const TripLegInfo({
    required this.placeName,
    required this.slug,
    required this.memberIds,
    this.centroid,
  });

  factory TripLegInfo.fromLeg(pb.TripLeg leg) {
    return TripLegInfo(
      placeName: leg.placeName,
      slug: leg.slug,
      memberIds: List<String>.unmodifiable(leg.imageIds),
      centroid: leg.hasCentroid()
          ? (leg.centroid.latitude, leg.centroid.longitude)
          : null,
    );
  }

  /// "Rome, Italy"; empty when the back end had no geo table.
  final String placeName;

  /// Filesystem-safe label, unique within the trip.
  final String slug;

  final List<String> memberIds;
  final (double, double)? centroid;
}

/// A spatiotemporal trip cluster: an away-from-home trip, or a per-month
/// bucket of at-home photos ([isHome]).
class TripInfo {
  const TripInfo({
    required this.id,
    required this.start,
    required this.end,
    required this.memberIds,
    this.centroid,
    this.folder,
    this.folderSlug = '',
    this.placeName = '',
    this.legs = const <TripLegInfo>[],
    this.isHome = false,
  });

  factory TripInfo.fromTrip(pb.Trip trip) {
    return TripInfo(
      id: trip.id,
      start: DateTime.fromMillisecondsSinceEpoch(trip.startUnixMs.toInt()),
      end: DateTime.fromMillisecondsSinceEpoch(trip.endUnixMs.toInt()),
      memberIds: List<String>.unmodifiable(trip.imageIds),
      centroid: trip.hasCentroid()
          ? (trip.centroid.latitude, trip.centroid.longitude)
          : null,
      folder: trip.folder.isEmpty ? null : trip.folder,
      folderSlug: trip.folderSlug,
      placeName: trip.placeName,
      legs: List<TripLegInfo>.unmodifiable(
        trip.legs.map(TripLegInfo.fromLeg),
      ),
      isHome: trip.isHome,
    );
  }

  final int id;
  final DateTime start;
  final DateTime end;

  /// Member image ids in chronological order.
  final List<String> memberIds;

  /// Mean of members with GPS; null when no member has GPS.
  final (double, double)? centroid;

  /// Display folder name from the back end ("Rome, Italy · April 2026", or
  /// "April 2026" without geo), or null when the back end gave none.
  final String? folder;

  /// Filesystem-safe form of [folder] used for the committed output layout.
  final String folderSlug;

  /// Dominant place ("Italy"); empty without a geo table or GPS.
  final String placeName;

  /// Contiguous legs; a single entry unless the trip visited several places.
  final List<TripLegInfo> legs;

  /// True when these are at-home photos rather than a trip.
  final bool isHome;

  TripInfo copyWith({
    DateTime? start,
    DateTime? end,
    List<String>? memberIds,
    (double, double)? centroid,
    List<TripLegInfo>? legs,
  }) {
    return TripInfo(
      id: id,
      start: start ?? this.start,
      end: end ?? this.end,
      memberIds: memberIds ?? this.memberIds,
      centroid: centroid ?? this.centroid,
      folder: folder,
      folderSlug: folderSlug,
      placeName: placeName,
      legs: legs ?? this.legs,
      isHome: isHome,
    );
  }
}

/// A named collection of trips, displayed as a collapsible folder
/// in the trips review UI (spec/frontend.md §6.2, §15).
class TripFolderInfo {
  const TripFolderInfo({
    required this.name,
    required this.trips,
  });

  final String name;

  /// Trips belonging to this folder, in their original order.
  final List<TripInfo> trips;
}

/// Errors as values. The GUI never throws operational failures.
sealed class BackendError implements Exception {
  const BackendError(this.message);

  final String message;

  @override
  String toString() => message;
}

/// The back-end process died or the transport dropped mid-call.
final class BackendCrashed extends BackendError {
  const BackendCrashed(super.message);
}

/// A gRPC call terminated with a non-cancel status.
final class BackendRpc extends BackendError {
  BackendRpc(this.error) : super(error.message ?? '');

  final GrpcError error;
}

/// The back-end could not be launched or signalled readiness.
final class BackendStartupFailed extends BackendError {
  const BackendStartupFailed(super.message);
}

/// A requested path or resource does not exist.
final class BackendNotFound extends BackendError {
  const BackendNotFound(super.message);
}

/// Maps gRPC statuses to [BackendError] values (spec/frontend.md §10.5).
///
/// `NOT_FOUND` becomes [BackendNotFound]; `UNAVAILABLE` during a call is
/// treated as a crash; anything else is surfaced verbatim as [BackendRpc].
BackendError mapGrpcError(GrpcError error) => switch (error.code) {
      StatusCode.notFound => BackendNotFound(error.message ?? ''),
      StatusCode.unavailable => BackendCrashed(error.message ?? ''),
      _ => BackendRpc(error),
    };
