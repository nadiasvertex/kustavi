// GENERATED CODE - DO NOT MODIFY BY HAND

part of 'log.dart';

// **************************************************************************
// RiverpodGenerator
// **************************************************************************

// GENERATED CODE - DO NOT MODIFY BY HAND
// ignore_for_file: type=lint, type=warning
/// Snapshot of the back end's stdout/stderr ring buffer (spec/frontend.md
/// §3.1, §9).

@ProviderFor(backendLog)
final backendLogProvider = BackendLogProvider._();

/// Snapshot of the back end's stdout/stderr ring buffer (spec/frontend.md
/// §3.1, §9).

final class BackendLogProvider
    extends $FunctionalProvider<String, String, String>
    with $Provider<String> {
  /// Snapshot of the back end's stdout/stderr ring buffer (spec/frontend.md
  /// §3.1, §9).
  BackendLogProvider._()
    : super(
        from: null,
        argument: null,
        retry: null,
        name: r'backendLogProvider',
        isAutoDispose: false,
        dependencies: null,
        $allTransitiveDependencies: null,
      );

  @override
  String debugGetCreateSourceHash() => _$backendLogHash();

  @$internal
  @override
  $ProviderElement<String> $createElement($ProviderPointer pointer) =>
      $ProviderElement(pointer);

  @override
  String create(Ref ref) {
    return backendLog(ref);
  }

  /// {@macro riverpod.override_with_value}
  Override overrideWithValue(String value) {
    return $ProviderOverride(
      origin: this,
      providerOverride: $SyncValueProvider<String>(value),
    );
  }
}

String _$backendLogHash() => r'de36e61962b63726b571058b0135cc4fdf27ee8d';
