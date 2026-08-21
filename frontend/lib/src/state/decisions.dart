import 'package:riverpod_annotation/riverpod_annotation.dart';

import 'domain.dart';

part 'decisions.g.dart';

/// User deletion intent, tracked as two isolated collections so explicit
/// choices survive automated pass defaults (spec/frontend.md §8).
///
/// All collections are immutable; every mutation returns a new instance so
/// Riverpod state replacement triggers rebuilds.
class DeletionIntent {
  const DeletionIntent({
    this.explicitKept = const <String>{},
    this.explicitDeleted = const <String>{},
    this.groupKeepers = const <int, String>{},
  });

  /// Images the user explicitly decided to keep.
  final Set<String> explicitKept;

  /// Images the user explicitly marked for deletion.
  final Set<String> explicitDeleted;

  /// User reassignments of a similar-group keeper: group id -> keeper image id.
  /// Absent groups fall back to the back end's recommendation.
  final Map<int, String> groupKeepers;

  bool isExplicitlyKept(String id) => explicitKept.contains(id);

  bool isExplicitlyDeleted(String id) => explicitDeleted.contains(id);

  /// The user-chosen keeper of [groupId], or null when unassigned.
  String? userKeeperOf(int groupId) => groupKeepers[groupId];

  DeletionIntent withToggle(String id) => isExplicitlyDeleted(id)
      ? withKept(id)
      : withMarked(id);

  DeletionIntent withKept(String id) {
    final kept = Set<String>.of(explicitKept)..add(id);
    final deleted = Set<String>.of(explicitDeleted)..remove(id);
    return DeletionIntent(explicitKept: kept, explicitDeleted: deleted, groupKeepers: groupKeepers);
  }

  DeletionIntent withMarked(String id) {
    final deleted = Set<String>.of(explicitDeleted)..add(id);
    final kept = Set<String>.of(explicitKept)..remove(id);
    return DeletionIntent(explicitKept: kept, explicitDeleted: deleted, groupKeepers: groupKeepers);
  }

  DeletionIntent withKeepAll(Iterable<String> ids) {
    final kept = Set<String>.of(explicitKept)..addAll(ids);
    final deleted = Set<String>.of(explicitDeleted)
      ..removeWhere(kept.contains);
    return DeletionIntent(explicitKept: kept, explicitDeleted: deleted, groupKeepers: groupKeepers);
  }

  DeletionIntent withMarkAll(Iterable<String> ids) {
    final deleted = Set<String>.of(explicitDeleted)..addAll(ids);
    final kept = Set<String>.of(explicitKept)
      ..removeWhere(deleted.contains);
    return DeletionIntent(explicitKept: kept, explicitDeleted: deleted, groupKeepers: groupKeepers);
  }

  DeletionIntent withKeeper(int groupId, String keeperId) {
    return DeletionIntent(
      explicitKept: explicitKept,
      explicitDeleted: explicitDeleted,
      groupKeepers: Map<int, String>.of(groupKeepers)..[groupId] = keeperId,
    );
  }

  DeletionIntent cleared() => const DeletionIntent();
}

/// Owns the user's explicit deletion intent (spec/frontend.md §9).
@Riverpod(keepAlive: true)
class DeletionPlan extends _$DeletionPlan {
  @override
  DeletionIntent build() => const DeletionIntent();

  void toggle(String id) => state = state.withToggle(id);

  void keep(String id) => state = state.withKept(id);

  void mark(String id) => state = state.withMarked(id);

  void keepAll(Iterable<String> ids) => state = state.withKeepAll(ids);

  void markAll(Iterable<String> ids) => state = state.withMarkAll(ids);

  void assignKeeper(int groupId, String keeperId) =>
      state = state.withKeeper(groupId, keeperId);

  /// Forgets all intent (new session / start over).
  void reset() => state = const DeletionIntent();
}

/// The automated-default context a step applies beneath explicit intent.
enum DeletionStep {
  /// S4 — deletion by default when flagged by back-end metrics.
  quality,

  /// S7 — deletion by default when flagged by Moondream inference.
  junk,

  /// S9 — deletion by default when not the designated group keeper.
  similar,
}

/// Evaluates the deterministic deletion hierarchy (spec/frontend.md §8):
///
/// 1. in [DeletionIntent.explicitDeleted] -> marked.
/// 2. in [DeletionIntent.explicitKept] -> not marked.
/// 3. step default: quality-flagged / junk-flagged / not the group keeper.
/// 4. otherwise not marked.
bool isMarkedForDeletion(
  DeletionIntent plan,
  String id, {
  required DeletionStep? step,
  required Set<String> qualityFlagged,
  required Set<String> junkFlagged,
  required Map<String, String> similarKeepers,
}) {
  if (plan.explicitDeleted.contains(id)) {
    return true;
  }
  if (plan.explicitKept.contains(id)) {
    return false;
  }
  return switch (step) {
    DeletionStep.quality => qualityFlagged.contains(id),
    DeletionStep.junk => junkFlagged.contains(id),
    DeletionStep.similar => similarKeepers[id]?.isNotEmpty == true &&
        similarKeepers[id] != id,
    null => false,
  };
}

/// Maps each group member to its designated keeper: the user's reassignment
/// when present, otherwise the back end's recommendation.
Map<String, String> similarKeeperMap(
  DeletionIntent plan,
  List<SimilarGroupInfo> groups,
) {
  final result = <String, String>{};
  for (final group in groups) {
    final keeper = plan.userKeeperOf(group.id) ?? group.recommendedKeepId;
    for (final member in group.memberIds) {
      result[member] = keeper;
    }
  }
  return result;
}
