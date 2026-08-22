import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:kustavi/src/backend/client_provider.dart';
import 'package:kustavi/src/state/model_status.dart';

import '../helpers.dart';

/// Flushed microtasks until [done] holds (provider streams are async).
Future<void> pumpUntil(
  ProviderContainer container,
  bool Function() done, {
  int maxIterations = 100,
}) async {
  for (var i = 0; i < maxIterations && !done(); i++) {
    await Future<void>.delayed(Duration.zero);
  }
}

void main() {
  group('modelStatusProvider (§6.3)', () {
    test('unknown → downloading → ready', () async {
      final client = FakeKustaviClient(
        modelEvents: [
          modelProgress(doneBytes: 600000000, totalBytes: 1200000000),
          modelReady(),
        ],
      );
      final container = ProviderContainer(
        overrides: [kustaviClientProvider.overrideWith((ref) => client)],
      );
      addTearDown(container.dispose);

      expect(
        container.read(modelStatusProvider).value,
        isA<ModelPrepUnknown>(),
      );
      await pumpUntil(
        container,
        () => container.read(modelStatusProvider).value is ModelPrepReady,
      );
      final state = container.read(modelStatusProvider).value
          as ModelPrepReady;
      expect(state.modelName, 'moondream-3.1');
    });

    test('download progress carries byte counters', () async {
      final client = FakeKustaviClient(
        modelEvents: [
          modelProgress(doneBytes: 600000000, totalBytes: 1200000000),
        ],
        modelStreamStaysOpen: true,
      );
      final container = ProviderContainer(
        overrides: [kustaviClientProvider.overrideWith((ref) => client)],
      );
      addTearDown(container.dispose);

      await pumpUntil(
        container,
        () => container.read(modelStatusProvider).value
            is ModelPrepDownloading,
      );
      final state = container.read(modelStatusProvider).value
          as ModelPrepDownloading;
      expect(state.doneBytes, 600000000);
      expect(state.totalBytes, 1200000000);
      expect(state.fraction, closeTo(0.5, 1e-9));

      // S5 [Cancel]: the provider resets to unknown.
      container.read(modelStatusProvider.notifier).cancelDownload();
      expect(
        container.read(modelStatusProvider).value,
        isA<ModelPrepUnknown>(),
      );
    });

    test('stream failure surfaces ModelPrepFailed (§6.2 S5)', () async {
      final client = FakeKustaviClient(
        ensureModelError: StateError('download failed'),
      );
      final container = ProviderContainer(
        overrides: [kustaviClientProvider.overrideWith((ref) => client)],
      );
      addTearDown(container.dispose);

      await pumpUntil(
        container,
        () => container.read(modelStatusProvider).value is ModelPrepFailed,
      );
      final state = container.read(modelStatusProvider).value
          as ModelPrepFailed;
      expect(state.message, contains('download failed'));
    });
  });
}
