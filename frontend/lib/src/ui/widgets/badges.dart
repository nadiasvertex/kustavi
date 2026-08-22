import 'package:flutter/material.dart';

/// Red "delete" tag on a cell currently marked for deletion
/// (spec/frontend.md §7.1).
class DeleteTag extends StatelessWidget {
  const DeleteTag({super.key});

  @override
  Widget build(BuildContext context) {
    return const _Pill(
      label: 'delete',
      color: Colors.red,
      onColor: Colors.white,
    );
  }
}

/// Small chip for a flag reason on a grid cell (e.g. "Blurry").
class ReasonChip extends StatelessWidget {
  const ReasonChip(this.label, {super.key});

  final String label;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
      decoration: BoxDecoration(
        color: Colors.black54,
        borderRadius: BorderRadius.circular(3),
      ),
      child: Text(
        label,
        style: const TextStyle(color: Colors.white, fontSize: 10),
        maxLines: 1,
        overflow: TextOverflow.ellipsis,
      ),
    );
  }
}

/// Amber badge for the designated keeper of a similar group.
class KeeperBadge extends StatelessWidget {
  const KeeperBadge({super.key, this.suggested = false});

  /// True when the badge marks the back end's pre-selected keeper.
  final bool suggested;

  @override
  Widget build(BuildContext context) {
    return _Pill(
      label: suggested ? 'suggested' : 'keeper',
      color: Colors.amber.shade600,
      onColor: Colors.black87,
    );
  }
}

class _Pill extends StatelessWidget {
  const _Pill({
    required this.label,
    required this.color,
    required this.onColor,
  });

  final String label;
  final Color color;
  final Color onColor;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
      decoration: BoxDecoration(
        color: color,
        borderRadius: BorderRadius.circular(4),
      ),
      child: Text(
        label,
        style: TextStyle(
          color: onColor,
          fontSize: 11,
          fontWeight: FontWeight.w600,
        ),
      ),
    );
  }
}
