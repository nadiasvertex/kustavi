import 'dart:convert';

/// A trip the user created by hand.
class UserTripEdit {
  const UserTripEdit({
    required this.id,
    required this.startMs,
    required this.folder,
    required this.placeName,
  });

  final int id;
  final int startMs;
  final String folder;
  final String placeName;
}

/// A folder name the user typed over a trip's generated one. [from] is the
/// generated name at the time, used to skip the rename when the trips pass
/// later produces a different trip under the same id.
class FolderRenameEdit {
  const FolderRenameEdit({
    required this.tripId,
    required this.from,
    required this.name,
  });

  final int tripId;
  final String from;
  final String name;
}

/// The user's hand edits to the trips layout, saved with the session so a
/// resume can lay them back over a fresh clustering. The back end stores the
/// encoded string without reading it.
class TripEdits {
  const TripEdits({
    this.membership = const <String, int>{},
    this.userTrips = const <UserTripEdit>[],
    this.renames = const <FolderRenameEdit>[],
    this.organizeIntoFolders = true,
  });

  /// Image id -> trip id, or -1 for "pulled out of every trip".
  final Map<String, int> membership;
  final List<UserTripEdit> userTrips;
  final List<FolderRenameEdit> renames;
  final bool organizeIntoFolders;

  bool get isEmpty =>
      membership.isEmpty &&
      userTrips.isEmpty &&
      renames.isEmpty &&
      organizeIntoFolders;

  /// Empty when there is nothing to save.
  String encode() {
    if (isEmpty) {
      return '';
    }
    return jsonEncode(<String, Object?>{
      'v': 1,
      'membership': membership,
      'userTrips': [
        for (final t in userTrips)
          {
            'id': t.id,
            'startMs': t.startMs,
            'folder': t.folder,
            'place': t.placeName,
          },
      ],
      'renames': [
        for (final r in renames)
          {'id': r.tripId, 'from': r.from, 'name': r.name},
      ],
      'organize': organizeIntoFolders,
    });
  }

  /// Null when [raw] is empty or not a valid encoding.
  static TripEdits? decode(String raw) {
    if (raw.isEmpty) {
      return null;
    }
    try {
      final json = jsonDecode(raw) as Map<String, dynamic>;
      if (json['v'] != 1) {
        return null;
      }
      return TripEdits(
        membership: {
          for (final e in (json['membership'] as Map<String, dynamic>).entries)
            e.key: e.value as int,
        },
        userTrips: [
          for (final t in json['userTrips'] as List<dynamic>)
            UserTripEdit(
              id: (t as Map<String, dynamic>)['id'] as int,
              startMs: t['startMs'] as int,
              folder: t['folder'] as String,
              placeName: t['place'] as String,
            ),
        ],
        renames: [
          for (final r in json['renames'] as List<dynamic>)
            FolderRenameEdit(
              tripId: (r as Map<String, dynamic>)['id'] as int,
              from: r['from'] as String,
              name: r['name'] as String,
            ),
        ],
        organizeIntoFolders: json['organize'] as bool? ?? true,
      );
    } on Object {
      return null;
    }
  }
}
