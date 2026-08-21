// GENERATED CODE - DO NOT MODIFY BY HAND

part of 'model_status.dart';

// **************************************************************************
// RiverpodGenerator
// **************************************************************************

// GENERATED CODE - DO NOT MODIFY BY HAND
// ignore_for_file: type=lint, type=warning
/// Tracks the background `EnsureModel` pipeline started on app start
/// (spec/frontend.md §6.3). `ready` is cached for the session; on a back-end
/// crash the provider rebuilds with the new endpoint and re-runs the
/// idempotent `EnsureModel`.

@ProviderFor(ModelStatus)
final modelStatusProvider = ModelStatusProvider._();

/// Tracks the background `EnsureModel` pipeline started on app start
/// (spec/frontend.md §6.3). `ready` is cached for the session; on a back-end
/// crash the provider rebuilds with the new endpoint and re-runs the
/// idempotent `EnsureModel`.
final class ModelStatusProvider
    extends $AsyncNotifierProvider<ModelStatus, ModelPrepState> {
  /// Tracks the background `EnsureModel` pipeline started on app start
  /// (spec/frontend.md §6.3). `ready` is cached for the session; on a back-end
  /// crash the provider rebuilds with the new endpoint and re-runs the
  /// idempotent `EnsureModel`.
  ModelStatusProvider._()
    : super(
        from: null,
        argument: null,
        retry: null,
        name: r'modelStatusProvider',
        isAutoDispose: false,
        dependencies: null,
        $allTransitiveDependencies: null,
      );

  @override
  String debugGetCreateSourceHash() => _$modelStatusHash();

  @$internal
  @override
  ModelStatus create() => ModelStatus();
}

String _$modelStatusHash() => r'e681f5674afea62f18bac77ebb51f77faf610b45';

/// Tracks the background `EnsureModel` pipeline started on app start
/// (spec/frontend.md §6.3). `ready` is cached for the session; on a back-end
/// crash the provider rebuilds with the new endpoint and re-runs the
/// idempotent `EnsureModel`.

abstract class _$ModelStatus extends $AsyncNotifier<ModelPrepState> {
  FutureOr<ModelPrepState> build();
  @$mustCallSuper
  @override
  WhenComplete runBuild() {
    final ref = this.ref as $Ref<AsyncValue<ModelPrepState>, ModelPrepState>;
    final element =
        ref.element
            as $ClassProviderElement<
              AnyNotifier<AsyncValue<ModelPrepState>, ModelPrepState>,
              AsyncValue<ModelPrepState>,
              Object?,
              Object?
            >;
    return element.handleCreate(ref, build);
  }
}
