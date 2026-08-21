// GENERATED CODE - DO NOT MODIFY BY HAND

part of 'channel.dart';

// **************************************************************************
// RiverpodGenerator
// **************************************************************************

// GENERATED CODE - DO NOT MODIFY BY HAND
// ignore_for_file: type=lint, type=warning
/// A loopback [ClientChannel] to the ready back end (spec/frontend.md §3.3).
///
/// Insecure credentials over 127.0.0.1 only; the GUI-generated auth token is
/// injected into the transmission metadata of every call by the
/// [KustaviClient] wrapper (see `client.dart`).

@ProviderFor(grpcChannel)
final grpcChannelProvider = GrpcChannelProvider._();

/// A loopback [ClientChannel] to the ready back end (spec/frontend.md §3.3).
///
/// Insecure credentials over 127.0.0.1 only; the GUI-generated auth token is
/// injected into the transmission metadata of every call by the
/// [KustaviClient] wrapper (see `client.dart`).

final class GrpcChannelProvider
    extends
        $FunctionalProvider<
          AsyncValue<ClientChannel>,
          ClientChannel,
          FutureOr<ClientChannel>
        >
    with $FutureModifier<ClientChannel>, $FutureProvider<ClientChannel> {
  /// A loopback [ClientChannel] to the ready back end (spec/frontend.md §3.3).
  ///
  /// Insecure credentials over 127.0.0.1 only; the GUI-generated auth token is
  /// injected into the transmission metadata of every call by the
  /// [KustaviClient] wrapper (see `client.dart`).
  GrpcChannelProvider._()
    : super(
        from: null,
        argument: null,
        retry: null,
        name: r'grpcChannelProvider',
        isAutoDispose: false,
        dependencies: null,
        $allTransitiveDependencies: null,
      );

  @override
  String debugGetCreateSourceHash() => _$grpcChannelHash();

  @$internal
  @override
  $FutureProviderElement<ClientChannel> $createElement(
    $ProviderPointer pointer,
  ) => $FutureProviderElement(pointer);

  @override
  FutureOr<ClientChannel> create(Ref ref) {
    return grpcChannel(ref);
  }
}

String _$grpcChannelHash() => r'1d0a28e5bcdf62a7d52f31343d1d60b7478d91d0';
