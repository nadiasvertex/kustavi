import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math';
import 'dart:typed_data';

import 'package:flutter_riverpod/flutter_riverpod.dart' show WidgetRef;
import 'package:grpc/grpc.dart';
import 'package:path/path.dart' as p;
import 'package:riverpod_annotation/riverpod_annotation.dart';

import '../generated/kustavi/service.pb.dart' as pb;
import '../generated/kustavi/service.pbgrpc.dart' as stub;
import '../state/domain.dart';
import 'client.dart' show authMetadata;
import 'client_provider.dart' show kustaviClientProvider;

part 'process.g.dart';

/// Capacity of the in-memory back-end log ring buffer (spec/frontend.md §3.1).
const int kBackendLogMaxBytes = 256 * 1024;

/// Back end stdout/stderr lines are captured here (spec/frontend.md §3.1).
class RingBuffer {
  RingBuffer({required this.maxBytes});

  final int maxBytes;
  final List<Uint8List> _chunks = [];
  int _size = 0;

  int get sizeBytes => _size;

  void addBytes(List<int> bytes) {
    _chunks.add(Uint8List.fromList(bytes));
    _size += bytes.length;
    _trim();
  }

  void addLine(String line) => addBytes(utf8.encode('$line\n'));

  void _trim() {
    while (_size > maxBytes && _chunks.length > 1) {
      _size -= _chunks.first.length;
      _chunks.removeAt(0);
    }
    if (_size > maxBytes && _chunks.isNotEmpty) {
      final first = _chunks.first;
      if (first.length <= _size - maxBytes) {
        _size -= first.length;
        _chunks.removeAt(0);
      } else {
        final dropped = _size - maxBytes;
        final sliced = Uint8List.fromList(first.sublist(dropped));
        _chunks[0] = sliced;
        _size = sliced.length;
      }
    }
  }

  String get text {
    final builder = BytesBuilder(copy: false);
    for (final chunk in _chunks) {
      builder.add(chunk);
    }
    return utf8.decode(builder.takeBytes(), allowMalformed: true);
  }
}

/// Wraps the launched back-end [Process]: ready handshake, log ring, exit
/// supervision, and teardown.
class ProcessHandle {
  ProcessHandle._(this._process, {required this.token}) {
    _pump();
  }

  /// A handle for a process that was never launched (tests, pre-launch
  /// endpoints). Its [exitCode] never completes and [kill] is a no-op.
  ProcessHandle.inactive({required this.token, required this.port})
    : _process = null;

  final Process? _process;

  /// Secure random string exchanged with the back end at launch.
  final String token;

  /// OS-assigned loopback port parsed from the ready line.
  int port = 0;

  final RingBuffer log = RingBuffer(maxBytes: kBackendLogMaxBytes);

  /// Set before a clean shutdown so the supervision logic does not treat the
  /// resulting exit as a crash.
  bool shutdownRequested = false;

  static const _readyPrefix = 'KUSTAVI-READY';
  static final _readyPattern = RegExp(r'^KUSTAVI-READY (\d+)\s*$');

  Future<int> get exitCode => _process?.exitCode ?? Completer<int>().future;

  /// Launches the binary, waits for the ready line, and verifies the server
  /// answers `GetInfo` (spec/frontend.md §3.1).
  static Future<ProcessHandle> launchAndVerify({
    required String binary,
    required String token,
    Duration readyTimeout = const Duration(seconds: 30),
  }) async {
    final process = await Process.start(binary, [
      'serve',
      '--listen',
      '127.0.0.1:0',
      '--token',
      token,
    ], mode: ProcessStartMode.normal);
    final handle = ProcessHandle._(process, token: token);
    final port = await handle._waitForReady(readyTimeout);
    await handle._probeReadiness(port);
    return handle;
  }

  Completer<int>? _readyCompleter;

  void _pump() {
    final process = _process;
    if (process == null) {
      return;
    }
    unawaited(
      process.stdout
          .transform(utf8.decoder)
          .transform(const LineSplitter())
          .forEach(_consumeStdoutLine),
    );
    unawaited(
      process.stderr
          .transform(utf8.decoder)
          .transform(const LineSplitter())
          .forEach(log.addLine),
    );
  }

  void _consumeStdoutLine(String line) {
    if (line.startsWith(_readyPrefix)) {
      final match = _readyPattern.firstMatch(line);
      if (match == null) {
        _failReady(
          BackendStartupFailed(
            'Back end emitted a malformed ready line: "$line"',
          ),
        );
        return;
      }
      port = int.parse(match.group(1)!);
      _completeReady(port);
    } else {
      log.addLine(line);
    }
  }

  void _completeReady(int? port) {
    final completer = _readyCompleter;
    if (completer != null && !completer.isCompleted && port != null) {
      completer.complete(port);
      _readyCompleter = null;
    }
  }

  void _failReady(BackendStartupFailed error) {
    final completer = _readyCompleter;
    if (completer != null && !completer.isCompleted) {
      completer.completeError(error);
      _readyCompleter = null;
    }
  }

  Future<int> _waitForReady(Duration timeout) async {
    final completer = Completer<int>();
    _readyCompleter = completer;
    unawaited(
      exitCode.then((code) {
        _failReady(
          BackendStartupFailed(
            'Back end process exited (code $code) before signalling readiness.',
          ),
        );
      }),
    );
    try {
      return await completer.future.timeout(
        timeout,
        onTimeout: () => throw const BackendStartupFailed(
          'Back end did not signal readiness in time.',
        ),
      );
    } finally {
      _readyCompleter = null;
    }
  }

  Future<void> _probeReadiness(int port) async {
    final channel = ClientChannel(
      '127.0.0.1',
      port: port,
      options: const ChannelOptions(credentials: ChannelCredentials.insecure()),
    );
    try {
      final client = stub.KustaviClient(channel);
      await client
          .getInfo(
            pb.GetInfoRequest(),
            options: CallOptions(
              metadata: authMetadata(token),
              timeout: const Duration(seconds: 5),
            ),
          )
          .timeout(const Duration(seconds: 5));
    } on GrpcError catch (error) {
      throw BackendStartupFailed(
        'Back end readiness probe failed: ${error.message ?? 'status ${error.code}'}',
      );
    } finally {
      await channel.shutdown();
    }
  }

  void kill() {
    final process = _process;
    if (process == null) {
      return;
    }
    try {
      process.kill(ProcessSignal.sigkill);
    } on ProcessException {
      // The process already exited.
    }
  }
}

/// The loopback endpoint of the ready back end.
class BackendEndpoint {
  const BackendEndpoint({
    required this.handle,
    required this.token,
    required this.port,
  });

  final ProcessHandle handle;
  final String token;
  final int port;
}

/// Discovers the back-end binary (spec/frontend.md §3.1):
///
/// 1. `KUSTAVI_BACKEND_PATH` (absolute path, development override).
/// 2. `kustavi-backend[.exe]` next to the running GUI executable.
///
/// Returns null when no candidate exists.
String? findBackendBinary({
  Map<String, String>? environment,
  String? executablePath,
}) {
  final env = environment ?? Platform.environment;
  final envPath = env['KUSTAVI_BACKEND_PATH'];
  if (envPath != null && envPath.isNotEmpty) {
    return File(envPath).existsSync() ? envPath : null;
  }
  final exe = executablePath ?? Platform.resolvedExecutable;
  final binaryName = Platform.isWindows
      ? 'kustavi-backend.exe'
      : 'kustavi-backend';
  final candidate = p.join(p.dirname(exe), binaryName);
  return File(candidate).existsSync() ? candidate : null;
}

/// A secure random hex string used as the gRPC auth token.
String generateAuthToken([Random? random]) {
  final r = random ?? Random.secure();
  const alphabet = '0123456789abcdef';
  return String.fromCharCodes(
    List<int>.generate(64, (_) => alphabet.codeUnitAt(r.nextInt(16))),
  );
}

/// Launches exactly one back-end process, runs the ready handshake, and
/// exposes the loopback endpoint (spec/frontend.md §3.1).
///
/// Unexpected process exit while the app is alive flips this provider into
/// an error state (crash dialog, §10.1); `retry` relaunches with a fresh
/// token.
@Riverpod(keepAlive: true)
class BackendProcess extends _$BackendProcess {
  ProcessHandle? _handle;

  @override
  FutureOr<BackendEndpoint> build() async {
    final token = generateAuthToken();
    final binary = findBackendBinary();
    if (binary == null) {
      throw const BackendStartupFailed(
        'Back end not found. Set KUSTAVI_BACKEND_PATH or install the back end '
        'next to the app.',
      );
    }
    final handle = await ProcessHandle.launchAndVerify(
      binary: binary,
      token: token,
    );
    _handle = handle;
    ref.onDispose(() {
      _handle = null;
      handle.kill();
    });
    unawaited(
      handle.exitCode.then((code) {
        if (!handle.shutdownRequested) {
          state = AsyncValue.error(
            BackendCrashed(
              'Back end process exited unexpectedly (code $code).',
            ),
            StackTrace.current,
          );
        }
      }),
    );
    return BackendEndpoint(handle: handle, token: token, port: handle.port);
  }

  /// Clean shutdown (spec/frontend.md §3.2): call `Shutdown`, wait up to 3 s
  /// for process exit, then kill if still alive.
  Future<void> quit() async {
    final handle = _handle;
    if (handle == null) {
      return;
    }
    handle.shutdownRequested = true;
    try {
      final client = ref.read(kustaviClientProvider);
      if (client case AsyncData(:final value)) {
        await value.shutdown();
      }
    } catch (error) {
      // Best effort: the kill below still applies.
    }
    try {
      await handle.exitCode.timeout(const Duration(seconds: 3));
    } on TimeoutException {
      handle.kill();
    }
  }
}

/// Shuts the back end down cleanly and quits the app (§3.2, §10.4, S13).
Future<void> exitApp(WidgetRef ref) async {
  await ref.read(backendProcessProvider.notifier).quit();
  exit(0);
}
