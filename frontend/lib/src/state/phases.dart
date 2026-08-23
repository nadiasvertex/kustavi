/// The wizard's six steps, in order (spec/frontend.md §6.1).
enum WizardStep {
  select('Select'),
  quality('Quality'),
  duplicates('Duplicates'),
  trips('Trips'),
  junk('Junk'),
  copy('Copy');

  const WizardStep(this.label);

  /// Display name in the step indicator.
  final String label;
}

/// Sealed wizard state machine. Each variant carries the data its screen
/// renders; transitions are owned by the wizard controller.
sealed class WizardPhase {
  const WizardPhase();

  /// The step indicator position (0-based); 6 = every step completed.
  int get stepIndex;
}

/// S0 — start.
final class WizardStart extends WizardPhase {
  const WizardStart();

  @override
  int get stepIndex => WizardStep.select.index;
}

/// S1 — folder scan in flight.
final class WizardScanning extends WizardPhase {
  const WizardScanning({
    required this.folder,
    this.filesSeen = 0,
    this.imagesFound = 0,
    this.currentPath = '',
  });

  final String folder;
  final int filesSeen;
  final int imagesFound;
  final String currentPath;

  WizardScanning copyWith({
    int? filesSeen,
    int? imagesFound,
    String? currentPath,
  }) {
    return WizardScanning(
      folder: folder,
      filesSeen: filesSeen ?? this.filesSeen,
      imagesFound: imagesFound ?? this.imagesFound,
      currentPath: currentPath ?? this.currentPath,
    );
  }

  @override
  int get stepIndex => WizardStep.select.index;
}

/// S0-B — a saved session was detected; the user chooses resume or fresh.
///
/// Requires the back end's `resumed_session`/`saved_wizard_phase` payload,
/// which the current wire contract does not carry yet; modeled here so the
/// state machine is complete.
final class WizardSessionRestore extends WizardPhase {
  const WizardSessionRestore({
    required this.folder,
    required this.imageCount,
    required this.savedStepIndex,
  });

  final String folder;
  final int imageCount;

  /// The phase index the back end recorded before the previous exit.
  final int savedStepIndex;

  @override
  int get stepIndex => WizardStep.select.index;
}

/// S2 — folder confirmed, scan results ready for review.
final class WizardConfirmFolder extends WizardPhase {
  const WizardConfirmFolder({
    required this.folder,
    required this.imageCount,
    this.scanErrors = const <String>[],
  });

  final String folder;
  final int imageCount;

  /// `"<id>: <reason>"` for unreadable image files.
  final List<String> scanErrors;

  @override
  int get stepIndex => WizardStep.select.index;
}

/// S1 exit variant — the folder contained no images.
final class WizardNoImages extends WizardPhase {
  const WizardNoImages({required this.folder});

  final String folder;

  @override
  int get stepIndex => WizardStep.select.index;
}

/// S3 — quality pass running.
final class WizardQualityRunning extends WizardPhase {
  const WizardQualityRunning({this.done = 0, this.total = 0});

  final int done;
  final int total;

  @override
  int get stepIndex => WizardStep.quality.index;
}

/// S4 — quality review of flagged candidates.
final class WizardQualityReview extends WizardPhase {
  const WizardQualityReview({required this.flaggedCount, required this.totalImages});

  final int flaggedCount;
  final int totalImages;

  @override
  int get stepIndex => WizardStep.quality.index;
}

/// S5 — junk preparation; awaiting the vision model download.
final class WizardJunkPrep extends WizardPhase {
  const WizardJunkPrep();

  @override
  int get stepIndex => WizardStep.junk.index;
}

/// S6 — junk pass running (Moondream LLM).
final class WizardJunkRunning extends WizardPhase {
  const WizardJunkRunning({this.done = 0, this.total = 0});

  final int done;
  final int total;

  @override
  int get stepIndex => WizardStep.junk.index;
}

/// S7 — junk review of LLM-flagged candidates.
final class WizardJunkReview extends WizardPhase {
  const WizardJunkReview({required this.flaggedCount, required this.totalImages});

  final int flaggedCount;
  final int totalImages;

  @override
  int get stepIndex => WizardStep.junk.index;
}

/// S8 — similar-image pass running.
final class WizardSimilarRunning extends WizardPhase {
  const WizardSimilarRunning({this.done = 0, this.total = 0});

  final int done;
  final int total;

  @override
  int get stepIndex => WizardStep.duplicates.index;
}

/// S9 — similar-group review.
final class WizardSimilarReview extends WizardPhase {
  const WizardSimilarReview({required this.groupCount, required this.markedCount});

  final int groupCount;
  final int markedCount;

  @override
  int get stepIndex => WizardStep.duplicates.index;
}

/// S10 — trips; spatiotemporal grouping with user thresholds.
final class WizardTrips extends WizardPhase {
  const WizardTrips({
    this.maxGapHours = 48,
    this.maxDistanceKm = 300,
  });

  final int maxGapHours;
  final int maxDistanceKm;

  @override
  int get stepIndex => WizardStep.trips.index;
}

/// S11 — commit summary.
final class WizardCommitSummary extends WizardPhase {
  const WizardCommitSummary({required this.keepCount, required this.keepBytes});

  final int keepCount;
  final int keepBytes;

  @override
  int get stepIndex => WizardStep.copy.index;
}

/// S12 — commit in flight.
final class WizardCommitting extends WizardPhase {
  const WizardCommitting({this.done = 0, this.total = 0});

  final int done;
  final int total;

  @override
  int get stepIndex => WizardStep.copy.index;
}

/// S13 — done.
final class WizardDone extends WizardPhase {
  const WizardDone({required this.copiedCount, required this.destination, this.errors = const <String>[]});

  final int copiedCount;
  final String destination;
  final List<String> errors;

  @override
  int get stepIndex => WizardStep.copy.index + 1;
}
