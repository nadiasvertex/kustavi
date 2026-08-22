import 'dart:async';

import 'package:fixnum/fixnum.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:grpc/grpc.dart';
import 'package:kustavi/src/backend/client.dart';
import 'package:kustavi/src/backend/client_provider.dart';
import 'package:kustavi/src/generated/kustavi/service.pb.dart';
import 'package:kustavi/src/state/phases.dart';
import 'package:kustavi/src/state/wizard.dart';

class DummyClient implements KustaviClient {
  @override
  Future<GetInfoResponse> getInfo() async => GetInfoResponse();

  @override
  Stream<ScanEvent> scanFolder(ScanFolderRequest request) {
    return Stream<ScanEvent>.fromIterable([
      ScanEvent()
        ..image = (ImageMeta()
          ..id = 'a.jpg'
          ..name = 'a.jpg'
          ..path = '/x/a.jpg'
          ..thumbnailPath = '/c/a.jpg'),
      ScanEvent()..complete = (ScanComplete()..images = 1),
    ]);
  }

  @override
  Stream<QualityEvent> runQualityPass() {
    final controller = StreamController<QualityEvent>();
    controller
      ..addError(
        GrpcError.custom(StatusCode.internal, 'quality exploded'),
      )
      ..close();
    return controller.stream;
  }

  @override
  Stream<ModelEvent> ensureModel() => Stream.empty();

  @override
  Stream<JunkEvent> runJunkPass() => Stream.empty();

  @override
  Stream<SimilarEvent> runSimilarPass() => Stream.empty();

  @override
  Stream<TripsEvent> runTripsPass(RunTripsPassRequest request) =>
      Stream.empty();

  @override
  Stream<CommitEvent> commit(CommitRequest request) => Stream.empty();

  @override
  Future<void> shutdown() async {}
}

void main() {
  test('stream semantics', () async {
    final controller = StreamController<int>();
    final sub = controller.stream.listen(
      (event) => print('got $event'),
      onError: (Object error, StackTrace stack) => print('err $error'),
      onDone: () => print('done'),
    );
    controller.add(1);
    await Future<void>.delayed(Duration.zero);
    controller.addError(StateError('boom'));
    await Future<void>.delayed(Duration.zero);
    await controller.close();
    await Future<void>.delayed(Duration.zero);
    await sub.cancel();
  });

  test('wizard quality error', () async {
    final container = ProviderContainer(
      overrides: [kustaviClientProvider.overrideWith((ref) => DummyClient())],
    );
    addTearDown(container.dispose);
    final wizard = container.read(wizardProvider.notifier);
    await Future<void>.delayed(Duration.zero);
    print('initial: ${container.read(wizardProvider)}');
    wizard.selectFolder('/x');
    for (var i = 0; i < 20; i++) {
      await Future<void>.delayed(Duration.zero);
    }
    print('after scan: ${container.read(wizardProvider)}');
    wizard.continueFromConfirm();
    for (var i = 0; i < 12; i++) {
      await Future<void>.delayed(Duration.zero);
      print('t$i: ${container.read(wizardProvider)}');
    }
  });
}
