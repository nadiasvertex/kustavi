// GENERATED CODE - DO NOT MODIFY BY HAND

part of 'decisions.dart';

// **************************************************************************
// RiverpodGenerator
// **************************************************************************

// GENERATED CODE - DO NOT MODIFY BY HAND
// ignore_for_file: type=lint, type=warning
/// Owns the user's explicit deletion intent (spec/frontend.md §9).

@ProviderFor(DeletionPlan)
final deletionPlanProvider = DeletionPlanProvider._();

/// Owns the user's explicit deletion intent (spec/frontend.md §9).
final class DeletionPlanProvider
    extends $NotifierProvider<DeletionPlan, DeletionIntent> {
  /// Owns the user's explicit deletion intent (spec/frontend.md §9).
  DeletionPlanProvider._()
    : super(
        from: null,
        argument: null,
        retry: null,
        name: r'deletionPlanProvider',
        isAutoDispose: false,
        dependencies: null,
        $allTransitiveDependencies: null,
      );

  @override
  String debugGetCreateSourceHash() => _$deletionPlanHash();

  @$internal
  @override
  DeletionPlan create() => DeletionPlan();

  /// {@macro riverpod.override_with_value}
  Override overrideWithValue(DeletionIntent value) {
    return $ProviderOverride(
      origin: this,
      providerOverride: $SyncValueProvider<DeletionIntent>(value),
    );
  }
}

String _$deletionPlanHash() => r'6cbc0b9bfa183c350179eaff341cef2430f764d1';

/// Owns the user's explicit deletion intent (spec/frontend.md §9).

abstract class _$DeletionPlan extends $Notifier<DeletionIntent> {
  DeletionIntent build();
  @$mustCallSuper
  @override
  WhenComplete runBuild() {
    final ref = this.ref as $Ref<DeletionIntent, DeletionIntent>;
    final element =
        ref.element
            as $ClassProviderElement<
              AnyNotifier<DeletionIntent, DeletionIntent>,
              DeletionIntent,
              Object?,
              Object?
            >;
    return element.handleCreate(ref, build);
  }
}
