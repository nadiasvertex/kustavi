import 'dart:async';

import 'package:grpc/grpc.dart';
import 'package:riverpod_annotation/riverpod_annotation.dart';

import 'process.dart';

part 'channel.g.dart';

/// A loopback [ClientChannel] to the ready back end (spec/frontend.md §3.3).
///
/// Insecure credentials over 127.0.0.1 only; the GUI-generated auth token is
/// injected into the transmission metadata of every call by the
/// [KustaviClient] wrapper (see `client.dart`).
@Riverpod(keepAlive: true)
FutureOr<ClientChannel> grpcChannel(Ref ref) async {
  final endpoint = await ref.watch(backendProcessProvider.future);
  final channel = ClientChannel(
    '127.0.0.1',
    port: endpoint.port,
    options: const ChannelOptions(credentials: ChannelCredentials.insecure()),
  );
  ref.onDispose(() {
    unawaited(channel.shutdown());
  });
  return channel;
}
