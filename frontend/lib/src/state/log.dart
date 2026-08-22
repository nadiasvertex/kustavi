import 'package:riverpod_annotation/riverpod_annotation.dart';

import '../backend/process.dart';

part 'log.g.dart';

/// Snapshot of the back end's stdout/stderr ring buffer (spec/frontend.md
/// §3.1, §9).
@Riverpod(keepAlive: true)
String backendLog(Ref ref) {
  final AsyncValue<BackendEndpoint> endpoint = ref
      .watch(backendProcessProvider);
  return endpoint.value?.handle.log.text ?? '';
}
