import 'dart:async';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart' hide ImageInfo;
import 'package:flutter/services.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../state/decisions.dart';
import '../../state/domain.dart';
import '../format.dart';

/// Opens the image detail modal (spec/frontend.md §7.2).
///
/// [canToggleDeletion] enables the deletion switch in S4/S7/S9/S10 and keeps
/// it read-only in S2.
Future<T?> showImageDetail<T extends Object?>(
  BuildContext context, {
  required ImageInfo image,
  required bool canToggleDeletion,
  DeletionStep? step,
  Set<String> qualityFlagged = const <String>{},
  Set<String> junkFlagged = const <String>{},
  Set<String> videoFlagged = const <String>{},
  Map<String, String> similarKeepers = const <String, String>{},
  double? sharpness,
  double? exposureScore,
  String? junkReason,
  double? junkConfidence,
  String? videoReason,
  double? videoConfidence,
  Map<String, String> mappings = const <String, String>{},
}) {
  return showDialog<T>(
    context: context,
    barrierColor: Colors.black,
    builder: (context) => Material(
      child: DetailView(
        image: image,
        canToggleDeletion: canToggleDeletion,
        step: step,
        qualityFlagged: qualityFlagged,
        junkFlagged: junkFlagged,
        videoFlagged: videoFlagged,
        similarKeepers: similarKeepers,
        sharpness: sharpness,
        exposureScore: exposureScore,
        junkReason: junkReason,
        junkConfidence: junkConfidence,
        videoReason: videoReason,
        videoConfidence: videoConfidence,
        mappings: mappings,
      ),
    ),
  );
}

/// Modal: the working preview shows instantly; the full-resolution master is
/// decoded off the UI thread and swapped in when ready. Zoom is 1×–8× with
/// double-click reset (spec/frontend.md §7.2).
class DetailView extends ConsumerStatefulWidget {
  const DetailView({
    super.key,
    required this.image,
    required this.canToggleDeletion,
    this.step,
    this.qualityFlagged = const <String>{},
    this.junkFlagged = const <String>{},
    this.videoFlagged = const <String>{},
    this.similarKeepers = const <String, String>{},
    this.sharpness,
    this.exposureScore,
    this.junkReason,
    this.junkConfidence,
    this.videoReason,
    this.videoConfidence,
    this.mappings = const <String, String>{},
  });

  final ImageInfo image;
  final bool canToggleDeletion;
  final DeletionStep? step;
  final Set<String> qualityFlagged;
  final Set<String> junkFlagged;
  final Set<String> videoFlagged;
  final Map<String, String> similarKeepers;
  final double? sharpness;
  final double? exposureScore;
  final String? junkReason;
  final double? junkConfidence;
  final String? videoReason;
  final double? videoConfidence;

  /// Extra label → value rows (trip, leg, place, group…) shown in the
  /// metadata panel (spec/frontend.md §7.2 "group/trip metadata mappings").
  final Map<String, String> mappings;

  @override
  ConsumerState<DetailView> createState() => _DetailViewState();
}

class _DetailViewState extends ConsumerState<DetailView>
    with SingleTickerProviderStateMixin {
  late final FileImage _workingProvider;
  ui.Image? _master;
  final TransformationController _transformation = TransformationController();
  late final AnimationController _resetController = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 200),
  );
  Animation<Matrix4>? _resetAnimation;
  final FocusNode _focus = FocusNode();

  @override
  void initState() {
    super.initState();
    _workingProvider = FileImage(File(widget.image.workingImagePath));
    _resetController.addListener(() {
      final animation = _resetAnimation;
      if (animation != null) {
        _transformation.value = animation.value;
      }
    });
    unawaited(_loadMaster());
  }

  /// Reads the full-resolution master and decodes it (the decode runs in the
  /// engine's thread pool via [ui.instantiateImageCodec]), then swaps it into
  /// the viewport (§7.2 progressive loading).
  Future<void> _loadMaster() async {
    try {
      final bytes = await File(widget.image.path).readAsBytes();
      final codec = await ui.instantiateImageCodec(bytes);
      if (!mounted) {
        return;
      }
      final frame = await codec.getNextFrame();
      if (!mounted) {
        return;
      }
      setState(() => _master = frame.image);
    } on Object {
      // Keep the working preview when the master fails to load.
    }
  }

  void _resetTransform() {
    if (_resetController.isAnimating) {
      return;
    }
    _resetAnimation = Matrix4Tween(
      begin: _transformation.value,
      end: Matrix4.identity(),
    ).animate(_resetController);
    unawaited(_resetController.forward());
  }

  void _close() => Navigator.of(context).pop();

  @override
  void dispose() {
    // Free the decoded master and evict the working preview from the image
    // cache so the modal cannot leak (§7.2, §11).
    _master?.dispose();
    PaintingBinding.instance.imageCache.evict(_workingProvider);
    _transformation.dispose();
    _resetController.dispose();
    _focus.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final plan = ref.watch(deletionPlanProvider);
    final marked = isMarkedForDeletion(
      plan,
      widget.image.id,
      step: widget.step,
      qualityFlagged: widget.qualityFlagged,
      junkFlagged: widget.junkFlagged,
      similarKeepers: widget.similarKeepers,
      videoFlagged: widget.videoFlagged,
    );
    return Focus(
      focusNode: _focus,
      autofocus: true,
      onKeyEvent: (node, event) {
        if (event is KeyDownEvent &&
            event.logicalKey == LogicalKeyboardKey.escape) {
          _close();
          return KeyEventResult.handled;
        }
        return KeyEventResult.ignored;
      },
      child: Stack(
        children: [
          // Click outside to close.
          Positioned.fill(
            child: GestureDetector(
              behavior: HitTestBehavior.opaque,
              onTap: _close,
            ),
          ),
          SafeArea(
            child: Padding(
              padding: const EdgeInsets.all(24),
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  SizedBox(width: 320, child: _metadataPanel(theme, marked)),
                  const SizedBox(width: 24),
                  Expanded(child: Center(child: _viewport())),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _viewport() {
    final master = _master;
    final width = widget.image.width.toDouble();
    final height = widget.image.height.toDouble();
    return GestureDetector(
      onDoubleTap: _resetTransform,
      child: InteractiveViewer(
        transformationController: _transformation,
        minScale: 1,
        maxScale: 8,
        child: FittedBox(
          fit: BoxFit.contain,
          child: AnimatedSwitcher(
            duration: const Duration(milliseconds: 200),
            child: master == null
                ? Image(
                    key: const ValueKey('working'),
                    image: _workingProvider,
                    width: width,
                    height: height,
                  )
                : RawImage(
                    key: const ValueKey('master'),
                    image: master,
                    width: width,
                    height: height,
                  ),
          ),
        ),
      ),
    );
  }

  Widget _metadataPanel(ThemeData theme, bool marked) {
    final image = widget.image;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(
          image.name,
          style: theme.textTheme.titleMedium,
          maxLines: 2,
          overflow: TextOverflow.ellipsis,
        ),
        const SizedBox(height: 12),
        _metaRow(theme, 'Path', image.path),
        _metaRow(theme, 'Dimensions', '${image.width} × ${image.height}'),
        _metaRow(theme, 'Size', formatBytes(image.sizeBytes)),
        if (image.taken != null)
          _metaRow(theme, 'Taken', formatDateTime(image.taken!)),
        if (image.gps != null)
          _metaRow(
            theme,
            'GPS',
            '${image.gps!.$1.toStringAsFixed(5)}, '
                '${image.gps!.$2.toStringAsFixed(5)}',
          ),
        if (widget.sharpness != null)
          _metaRow(theme, 'Sharpness', widget.sharpness!.toStringAsFixed(1)),
        if (widget.exposureScore != null)
          _metaRow(theme, 'Exposure', widget.exposureScore!.toStringAsFixed(2)),
        if (widget.junkReason != null)
          _metaRow(theme, 'Junk reason', widget.junkReason!),
        if (widget.junkConfidence != null)
          _metaRow(
            theme,
            'Confidence',
            '${(widget.junkConfidence! * 100).round()}%',
          ),
        if (widget.videoReason != null)
          _metaRow(theme, 'Video reason', widget.videoReason!),
        if (widget.videoConfidence != null)
          _metaRow(
            theme,
            'Confidence',
            '${(widget.videoConfidence! * 100).round()}%',
          ),
        for (final entry in widget.mappings.entries)
          if (entry.value.isNotEmpty) _metaRow(theme, entry.key, entry.value),
        const Spacer(),
        Row(
          children: [
            const Expanded(child: Text('Marked for deletion')),
            Switch(
              value: marked,
              onChanged: widget.canToggleDeletion
                  ? (value) =>
                        ref.read(deletionPlanProvider.notifier).toggle(image.id)
                  : null,
            ),
          ],
        ),
        const SizedBox(height: 8),
        FilledButton.tonalIcon(
          onPressed: _close,
          icon: const Icon(Icons.close),
          label: const Text('Close'),
        ),
      ],
    );
  }

  Widget _metaRow(ThemeData theme, String label, String value) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 8),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(
            label.toUpperCase(),
            style: theme.textTheme.labelSmall?.copyWith(
              color: theme.colorScheme.outline,
            ),
          ),
          const SizedBox(height: 2),
          Text(
            value,
            style: theme.textTheme.bodyMedium,
            maxLines: 2,
            overflow: TextOverflow.ellipsis,
          ),
        ],
      ),
    );
  }
}
