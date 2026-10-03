part of 'wizard.dart';

/// Trips pass: results, folder plan and per-photo curation.
extension WizardTrips on Wizard {
  /// Effective trips: the clustering result plus hand-created trips, with
  /// per-image reassignments applied, sorted chronologically. Empty trips
  /// (all members moved away) are dropped.
  List<TripInfo> get tripResults {
    final deleted = _deletedImageIds();
    final trips = <TripInfo>[];
    for (final base in [..._tripResults, ..._userTrips]) {
      final ids = _effectiveMemberIds(base, deleted);
      if (ids.isEmpty) {
        continue;
      }
      trips.add(_rebuildTrip(base, ids));
    }
    trips.sort((a, b) => a.start.compareTo(b.start));
    return List<TripInfo>.unmodifiable(trips);
  }

  bool get organizeIntoTripFolders => _organizeIntoTripFolders;
  set organizeIntoTripFolders(bool value) {
    _organizeIntoTripFolders = value;
    _publishTripsReviewPhase();
  }

  int get tripGapHours => _tripGapHours;
  int get tripDistanceKm => _tripDistanceKm;
  int get tripHomeRadiusKm => _tripHomeRadiusKm;
  int get tripLegRadiusKm => _tripLegRadiusKm;

  List<String> _effectiveMemberIds(TripInfo base, Set<String> deleted) {
    final ids = <String>{};
    for (final id in base.memberIds) {
      if (deleted.contains(id)) {
        continue;
      }
      if ((_tripMembership[id] ?? base.id) == base.id) {
        ids.add(id);
      }
    }
    _tripMembership.forEach((id, tid) {
      if (tid == base.id && !deleted.contains(id)) {
        ids.add(id);
      }
    });
    final ordered = ids.toList()
      ..sort((a, b) {
        final ta = _images[a]?.taken;
        final tb = _images[b]?.taken;
        if (ta != null && tb != null && ta != tb) {
          return ta.compareTo(tb);
        }
        return a.compareTo(b);
      });
    return ordered;
  }

  TripInfo _rebuildTrip(TripInfo base, List<String> ids) {
    final idSet = ids.toSet();
    // Untouched trip: keep the back end's start/end/centroid/legs verbatim.
    if (idSet.length == base.memberIds.length &&
        idSet.containsAll(base.memberIds)) {
      return base;
    }
    DateTime? first;
    DateTime? last;
    double latSum = 0;
    double lonSum = 0;
    int gps = 0;
    for (final id in ids) {
      final img = _images[id];
      if (img == null) {
        continue;
      }
      final taken = img.taken;
      if (taken != null) {
        if (first == null || taken.isBefore(first)) first = taken;
        if (last == null || taken.isAfter(last)) last = taken;
      }
      final g = img.gps;
      if (g != null) {
        latSum += g.$1;
        lonSum += g.$2;
        gps++;
      }
    }
    final centroid = gps > 0 ? (latSum / gps, lonSum / gps) : null;

    // Members were only removed (photos marked for deletion, or pulled out of
    // the trip): keep the back end's leg split and its geocoded place names,
    // just dropping the removed ids from each leg.
    final onlyRemovals = base.memberIds.toSet().containsAll(idSet);
    if (onlyRemovals && base.legs.length > 1) {
      final legs = <TripLegInfo>[];
      for (final leg in base.legs) {
        final kept = leg.memberIds
            .where(idSet.contains)
            .toList(growable: false);
        if (kept.isEmpty) {
          continue;
        }
        legs.add(
          TripLegInfo(
            placeName: leg.placeName,
            slug: leg.slug,
            memberIds: List<String>.unmodifiable(kept),
            centroid: leg.centroid,
          ),
        );
      }
      return base.copyWith(
        start: first ?? base.start,
        end: last ?? base.end,
        memberIds: List<String>.unmodifiable(ids),
        centroid: centroid,
        legs: legs,
      );
    }

    return base.copyWith(
      start: first ?? base.start,
      end: last ?? base.end,
      memberIds: List<String>.unmodifiable(ids),
      centroid: centroid,
      // Hand edits invalidate the back end's leg split; collapse to one leg.
      legs: <TripLegInfo>[
        TripLegInfo(
          placeName: base.placeName,
          slug: base.folderSlug.isNotEmpty ? base.folderSlug : 'leg-1',
          memberIds: List<String>.unmodifiable(ids),
          centroid: centroid,
        ),
      ],
    );
  }

  /// Effective folder name for a trip: user-renamed, or auto-generated.
  String _effectiveFolderOf(TripInfo trip) {
    return _tripFolderNames[trip.id] ?? (trip.folder ?? '');
  }

  /// Trips grouped into named folders, sorted by folder name then by start date.
  List<TripFolderInfo> get tripFolders {
    final Map<String, List<TripInfo>> byFolder = {};
    for (final trip in tripResults) {
      final name = _effectiveFolderOf(trip);
      byFolder.putIfAbsent(name, () => []).add(trip);
    }
    final sortedNames = List<String>.from(byFolder.keys)..sort();
    return sortedNames
        .map((name) => TripFolderInfo(name: name, trips: byFolder[name]!))
        .toList(growable: false);
  }

  /// Image ids not in any effective trip and not already marked for deletion
  /// (never-clustered photos plus any the user pulled out of a trip).
  List<String> get unassignedTripImageIds {
    final deleted = _deletedImageIds();
    final assigned = <String>{};
    for (final trip in tripResults) {
      assigned.addAll(trip.memberIds);
    }
    return _orderedIds
        .where((id) => !assigned.contains(id) && !deleted.contains(id))
        .toList(growable: false);
  }

  /// Reassigns [imageIds] to [tripId] (null → pull them out of every trip).
  void moveImagesToTrip(Iterable<String> imageIds, int? tripId) {
    for (final id in imageIds) {
      _tripMembership[id] = tripId ?? Wizard._kUnassignedTrip;
    }
    _publishTripsReviewPhase();
  }

  /// Creates a new trip seeded with [imageIds]; returns its id.
  int createTripFromImages(Iterable<String> imageIds) {
    final ids = imageIds.toList(growable: false);
    final id = _nextUserTripId++;
    DateTime? first;
    for (final imgId in ids) {
      final taken = _images[imgId]?.taken;
      if (taken != null && (first == null || taken.isBefore(first))) {
        first = taken;
      }
    }
    final anchor = first ?? DateTime.now();

    // The front end has no geo table; borrow a place name from the back end's
    // original clustering when the seed photos came from a geocoded trip/leg.
    final place = _dominantKnownPlace(ids);
    final String folder;
    if (place != null) {
      folder = '$place · ${Wizard._monthYear(anchor)}';
    } else if (first != null) {
      folder = 'Trip · ${Wizard._monthYear(anchor)}';
    } else {
      folder = 'New trip';
    }

    _userTrips.add(
      TripInfo(
        id: id,
        start: anchor,
        end: anchor,
        memberIds: const <String>[],
        folder: folder,
        placeName: place ?? '',
      ),
    );
    for (final imgId in ids) {
      _tripMembership[imgId] = id;
    }
    _publishTripsReviewPhase();
    return id;
  }

  /// The place name most of [imageIds] were originally clustered under (the
  /// leg's when known, else the trip's), or null when none were geocoded.
  String? _dominantKnownPlace(List<String> imageIds) {
    final counts = <String, int>{};
    for (final imgId in imageIds) {
      final place = _knownPlaceOf(imgId);
      if (place != null) {
        counts.update(place, (n) => n + 1, ifAbsent: () => 1);
      }
    }
    if (counts.isEmpty) {
      return null;
    }
    return counts.entries.reduce((a, b) => b.value > a.value ? b : a).key;
  }

  /// The geocoded place [imageId] was clustered under by the trips pass: its
  /// leg's place when the trip has legs, otherwise the trip's. Empty when the
  /// pass ran without a place table or the photo was never clustered.
  String? _knownPlaceOf(String imageId) {
    for (final trip in _tripResults) {
      for (final leg in trip.legs) {
        if (leg.placeName.isNotEmpty && leg.memberIds.contains(imageId)) {
          return leg.placeName;
        }
      }
      if (trip.placeName.isNotEmpty && trip.memberIds.contains(imageId)) {
        return trip.placeName;
      }
    }
    return null;
  }

  /// Per-image destination sub-path for `CommitRequest.folderForId`, built
  /// from the effective trip/leg layout. Empty when the user opted out.
  Map<String, String> commitFolderPlan() {
    if (!_organizeIntoTripFolders) {
      return const <String, String>{};
    }
    final plan = <String, String>{};
    for (final trip in tripResults) {
      // Prefer the back end's geocoded slug ("rome-italy-2026-04"); only fall
      // back to slugifying the display label when the user renamed the folder
      // or the trips pass had no place table.
      final renamed = _tripFolderNames.containsKey(trip.id);
      final tripSlug = renamed
          ? Wizard._slugify(_tripFolderNames[trip.id]!)
          : (trip.folderSlug.isNotEmpty
                ? trip.folderSlug
                : Wizard._slugify(_effectiveFolderOf(trip)));
      if (tripSlug.isEmpty) {
        continue;
      }
      if (trip.legs.length > 1) {
        for (final leg in trip.legs) {
          final legSlug = leg.slug.isNotEmpty
              ? Wizard._slugify(leg.slug)
              : 'leg';
          for (final id in leg.memberIds) {
            plan[id] = '$tripSlug/$legSlug';
          }
        }
      }
      for (final id in trip.memberIds) {
        plan.putIfAbsent(id, () => tripSlug);
      }
    }
    return plan;
  }

  /// Renames the folder that [tripId] belongs to to [newName].
  void renameTripFolder(int tripId, String newName) {
    if (newName.isEmpty) {
      return;
    }
    _tripFolderNames[tripId] = newName;
    _publishTripsReviewPhase();
  }

  void _resetTripEdits() {
    _tripMembership.clear();
    _userTrips.clear();
    _tripFolderNames.clear();
    _nextUserTripId = 1000000;
  }

  void _publishTripsReviewPhase() {
    if (_state.value is WizardTripsReview) {
      _state = AsyncValue.data(_tripsReviewPhase);
    }
  }

  void _startTripsPass() {
    _tripResults.clear();
    _resetTripEdits();
    _state = const AsyncValue.data(WizardTripsRunning());
    final client = _ref.read(kustaviClientProvider).requireValue;
    _subscribe(
      client.runTripsPass(_tripsRequest()),
      _onTripsEvent,
      _onTripsDone,
    );
  }

  /// Organize -> batch menu: the folders as they stand now become the batches.
  void continueFromTrips() {
    if (_state.value is! WizardTripsReview) {
      return;
    }
    _enterBatchMode();
    _state = AsyncValue.data(_batchMenuPhase);
  }

  void cancelTrips() {
    if (_state.value is! WizardTripsRunning) {
      return;
    }
    _cancelPass();
    _state = AsyncValue.data(_returnPhase ?? const WizardStart());
  }

  pb.RunTripsPassRequest _tripsRequest() {
    return pb.RunTripsPassRequest()
      ..maxGapHours = _tripGapHours
      ..maxDistanceKm = _tripDistanceKm
      ..homeRadiusKm = _tripHomeRadiusKm
      ..legRadiusKm = _tripLegRadiusKm;
  }

  WizardTripsReview get _tripsReviewPhase {
    final trips = tripResults;
    return WizardTripsReview(
      tripCount: trips.length,
      tripFolders: tripFolders,
      markedCount: _tripsMarkedCount(),
      trips: trips,
      unassignedCount: unassignedTripImageIds.length,
    );
  }

  /// Re-publishes S10 after the deletion plan changed under it (e.g. a photo
  /// toggled for deletion from the detail view).
  void refreshTripsReview() => _publishTripsReviewPhase();

  /// Photos the back end clustered into a trip that the user has since marked
  /// for deletion (by any step). Shown as the S10 "marked" stat.
  int _tripsMarkedCount() {
    final deleted = _deletedImageIds();
    if (deleted.isEmpty) {
      return 0;
    }
    final clustered = <String>{};
    for (final base in _tripResults) {
      clustered.addAll(base.memberIds);
    }
    return clustered.intersection(deleted).length;
  }

  void _onTripsEvent(pb.TripsEvent event) {
    if (_state.value is! WizardTripsRunning) {
      return;
    }
    switch (event.whichEvent()) {
      case pb.TripsEvent_Event.progress:
        _state = AsyncValue.data(
          WizardTripsRunning(
            done: event.progress.done,
            total: event.progress.total,
          ),
        );
      case pb.TripsEvent_Event.trip:
        _tripResults.add(TripInfo.fromTrip(event.trip));
      case pb.TripsEvent_Event.complete:
        break;
      case pb.TripsEvent_Event.notSet:
        break;
    }
  }

  void _onTripsDone() {
    if (_state.value is! WizardTripsRunning) {
      return;
    }
    final target = _resumeTargetStep;
    if (target != null) {
      // A resumed session re-runs the (cheap) trips pass, then lands where it
      // left off: the commit summary, or the batch menu for anything earlier.
      _resumeTargetStep = null;
      _enterBatchMode();
      _state = AsyncValue.data(
        target == WizardStep.copy.index ? _commitSummaryPhase : _batchMenuPhase,
      );
      return;
    }
    _state = AsyncValue.data(_tripsReviewPhase);
  }

  /// Organize [Back]: to the folder confirmation.
  void backFromTrips() {
    if (_state.value is! WizardTripsReview) {
      return;
    }
    _state = AsyncValue.data(_returnPhase ?? const WizardStart());
  }
}
