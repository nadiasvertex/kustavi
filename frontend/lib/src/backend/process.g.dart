// GENERATED CODE - DO NOT MODIFY BY HAND

part of 'process.dart';

// **************************************************************************
// RiverpodGenerator
// **************************************************************************

// GENERATED CODE - DO NOT MODIFY BY HAND
// ignore_for_file: type=lint, type=warning
/// Launches exactly one back-end process, runs the ready handshake, and
/// exposes the loopback endpoint (spec/frontend.md §3.1).
///
/// Unexpected process exit while the app is alive flips this provider into
/// an error state (crash dialog, §10.1); `retry` relaunches with a fresh
/// token.

@ProviderFor(BackendProcess)
final backendProcessProvider = BackendProcessProvider._();

/// Launches exactly one back-end process, runs the ready handshake, and
/// exposes the loopback endpoint (spec/frontend.md §3.1).
///
/// Unexpected process exit while the app is alive flips this provider into
/// an error state (crash dialog, §10.1); `retry` relaunches with a fresh
/// token.
final class BackendProcessProvider
    extends $AsyncNotifierProvider<BackendProcess, BackendEndpoint> {
  /// Launches exactly one back-end process, runs the ready handshake, and
  /// exposes the loopback endpoint (spec/frontend.md §3.1).
  ///
  /// Unexpected process exit while the app is alive flips this provider into
  /// an error state (crash dialog, §10.1); `retry` relaunches with a fresh
  /// token.
  BackendProcessProvider._()
    : super(
        from: null,
        argument: null,
        retry: null,
        name: r'backendProcessProvider',
        isAutoDispose: false,
        dependencies: null,
        $allTransitiveDependencies: null,
      );

  @override
  String debugGetCreateSourceHash() => _$backendProcessHash();

  @$internal
  @override
  BackendProcess create() => BackendProcess();
}

String _$backendProcessHash() => r'd72203d52a812f6a139cc3a8e65ae3989a090aff';

/// Launches exactly one back-end process, runs the ready handshake, and
/// exposes the loopback endpoint (spec/frontend.md §3.1).
///
/// Unexpected process exit while the app is alive flips this provider into
/// an error state (crash dialog, §10.1); `retry` relaunches with a fresh
/// token.

abstract class _$BackendProcess extends $AsyncNotifier<BackendEndpoint> {
  FutureOr<BackendEndpoint> build();
  @$mustCallSuper
  @override
  WhenComplete runBuild() {
    final ref = this.ref as $Ref<AsyncValue<BackendEndpoint>, BackendEndpoint>;
    final element =
        ref.element
            as $ClassProviderElement<
              AnyNotifier<AsyncValue<BackendEndpoint>, BackendEndpoint>,
              AsyncValue<BackendEndpoint>,
              Object?,
              Object?
            >;
    return element.handleCreate(ref, build);
  }
}
