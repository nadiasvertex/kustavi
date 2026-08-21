import 'dart:async';

import 'package:riverpod_annotation/riverpod_annotation.dart';

import '../backend/client_provider.dart';
import '../generated/kustavi/service.pb.dart' as pb;
import 'domain.dart';

part 'model_status.g.dart';

/// Vision model preparation states (spec/frontend.md §6.3):
/// `unknown -> downloading(progress) -> ready` or `failed(message)`.
sealed class ModelPrepState {
  const ModelPrepState();
}

final class ModelPrepUnknown extends ModelPrepState {
  const ModelPrepUnknown();
}

final class ModelPrepDownloading extends ModelPrepState {
  const ModelPrepDownloading({
    required this.doneBytes,
    required this.totalBytes,
    required this.speedBps,
  });

  final int doneBytes;

  /// 0 when the back end has not reported a total yet.
  final int totalBytes;
  final double speedBps;

  /// 0..1, or null when the total is unknown.
  double? get fraction => totalBytes > 0 ? doneBytes / totalBytes : null;
}

final class ModelPrepReady extends ModelPrepState {
  const ModelPrepReady({required this.modelName, required this.sizeBytes});

  final String modelName;
  final int sizeBytes;
}

final class ModelPrepFailed extends ModelPrepState {
  const ModelPrepFailed(this.message);

  final String message;
}

/// Tracks the background `EnsureModel` pipeline started on app start
/// (spec/frontend.md §6.3). `ready` is cached for the session; on a back-end
/// crash the provider rebuilds with the new endpoint and re-runs the
/// idempotent `EnsureModel`.
@Riverpod(keepAlive: true)
class ModelStatus extends _$ModelStatus {
  StreamSubscription<pb.ModelEvent>? _subscription;

  @override
  FutureOr<ModelPrepState> build() async {
    final client = await ref.watch(kustaviClientProvider.future);
    final subscription = client.ensureModel().listen(
      (event) {
        final next = switch (event.whichEvent()) {
          pb.ModelEvent_Event.ready => ModelPrepReady(
              modelName: event.ready.modelName,
              sizeBytes: event.ready.sizeBytes.toInt(),
            ),
          pb.ModelEvent_Event.progress => ModelPrepDownloading(
              doneBytes: event.progress.doneBytes.toInt(),
              totalBytes: event.progress.totalBytes.toInt(),
              speedBps: event.progress.speedBps,
            ),
          pb.ModelEvent_Event.notSet => null,
        };
        if (next != null) {
          state = AsyncValue.data(next);
        }
      },
      onError: (Object error, StackTrace _) {
        // A crashed back end re-runs this provider on retry; the download
        // failure itself is surfaced (S5, §6.2) and re-requested there.
        if (error is BackendCrashed) {
          return;
        }
        state = AsyncValue.data(
          ModelPrepFailed(error is BackendError ? error.message : error.toString()),
        );
      },
    );
    _subscription = subscription;
    ref.onDispose(() {
      unawaited(subscription.cancel());
    });
    return const ModelPrepUnknown();
  }

  /// Cancels the in-flight download (S5 [Cancel]); the next `EnsureModel`
  /// run resumes or restarts it.
  void cancelDownload() {
    unawaited(_subscription?.cancel());
    _subscription = null;
    state = const AsyncValue.data(ModelPrepUnknown());
  }

  /// Re-runs `EnsureModel` (S5 [Retry download]).
  void retryDownload() {
    ref.invalidateSelf();
  }
}
