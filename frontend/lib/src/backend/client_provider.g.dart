// GENERATED CODE - DO NOT MODIFY BY HAND

part of 'client_provider.dart';

// **************************************************************************
// RiverpodGenerator
// **************************************************************************

// GENERATED CODE - DO NOT MODIFY BY HAND
// ignore_for_file: type=lint, type=warning
/// The [KustaviClient] used across the app (spec/frontend.md §4, §9).
///
/// In tests this is overridden with a [FakeKustaviClient].

@ProviderFor(kustaviClient)
final kustaviClientProvider = KustaviClientProvider._();

/// The [KustaviClient] used across the app (spec/frontend.md §4, §9).
///
/// In tests this is overridden with a [FakeKustaviClient].

final class KustaviClientProvider
    extends
        $FunctionalProvider<
          AsyncValue<KustaviClient>,
          KustaviClient,
          FutureOr<KustaviClient>
        >
    with $FutureModifier<KustaviClient>, $FutureProvider<KustaviClient> {
  /// The [KustaviClient] used across the app (spec/frontend.md §4, §9).
  ///
  /// In tests this is overridden with a [FakeKustaviClient].
  KustaviClientProvider._()
    : super(
        from: null,
        argument: null,
        retry: null,
        name: r'kustaviClientProvider',
        isAutoDispose: false,
        dependencies: null,
        $allTransitiveDependencies: null,
      );

  @override
  String debugGetCreateSourceHash() => _$kustaviClientHash();

  @$internal
  @override
  $FutureProviderElement<KustaviClient> $createElement(
    $ProviderPointer pointer,
  ) => $FutureProviderElement(pointer);

  @override
  FutureOr<KustaviClient> create(Ref ref) {
    return kustaviClient(ref);
  }
}

String _$kustaviClientHash() => r'eb1c0cbdaafeddd923c44b5f876cf75e0e168d64';
