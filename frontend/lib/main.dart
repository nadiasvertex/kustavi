import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:fvp/fvp.dart' as fvp;

import 'app.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  // Backs video_player with libmpv on platforms without an official
  // implementation (Windows, Linux); macOS/iOS/Android keep their native
  // AVFoundation/ExoPlayer backends untouched.
  fvp.registerWith();
  runApp(const ProviderScope(child: KustaviApp()));
}
