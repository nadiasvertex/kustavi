import 'dart:ui' show AppExitResponse;

import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import 'src/backend/process.dart';
import 'src/ui/wizard_shell.dart';

/// The root [MaterialApp] with the app theming (spec/frontend.md §14).
class KustaviApp extends ConsumerStatefulWidget {
  const KustaviApp({super.key});

  @override
  ConsumerState<KustaviApp> createState() => _KustaviAppState();
}

class _KustaviAppState extends ConsumerState<KustaviApp>
    with WidgetsBindingObserver {
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    super.dispose();
  }

  /// The OS asked the app to terminate (window close button, Cmd/Ctrl+Q,
  /// log-out). Shut the back end down cleanly before we go so no process is
  /// orphaned (spec/frontend.md §3.2); the back end's parent-PID watchdog is
  /// the backstop for exits that never reach here (crash, SIGKILL).
  @override
  Future<AppExitResponse> didRequestAppExit() async {
    await ref.read(backendProcessProvider.notifier).quit();
    return AppExitResponse.exit;
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Kustavi',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: const Color(0xFF3E6B54)),
        useMaterial3: true,
      ),
      home: const WizardShell(),
    );
  }
}
