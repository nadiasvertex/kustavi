import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import 'src/ui/wizard_shell.dart';

/// The root [MaterialApp] with the app theming (spec/frontend.md §14).
class KustaviApp extends ConsumerWidget {
  const KustaviApp({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
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
