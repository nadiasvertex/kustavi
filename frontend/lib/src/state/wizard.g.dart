// GENERATED CODE - DO NOT MODIFY BY HAND

part of 'wizard.dart';

// **************************************************************************
// RiverpodGenerator
// **************************************************************************

// GENERATED CODE - DO NOT MODIFY BY HAND
// ignore_for_file: type=lint, type=warning
/// The wizard controller (spec/frontend.md §6, §9).
///
/// Owns the incremental image index (the GUI's single source of truth for
/// image metadata, §5) and the linear phase machine. One pass stream is in
/// flight at a time; `EnsureModel` is exempt (it runs in [ModelStatus]).

@ProviderFor(Wizard)
final wizardProvider = WizardProvider._();

/// The wizard controller (spec/frontend.md §6, §9).
///
/// Owns the incremental image index (the GUI's single source of truth for
/// image metadata, §5) and the linear phase machine. One pass stream is in
/// flight at a time; `EnsureModel` is exempt (it runs in [ModelStatus]).
final class WizardProvider extends $NotifierProvider<Wizard, WizardPhase> {
  /// The wizard controller (spec/frontend.md §6, §9).
  ///
  /// Owns the incremental image index (the GUI's single source of truth for
  /// image metadata, §5) and the linear phase machine. One pass stream is in
  /// flight at a time; `EnsureModel` is exempt (it runs in [ModelStatus]).
  WizardProvider._()
    : super(
        from: null,
        argument: null,
        retry: null,
        name: r'wizardProvider',
        isAutoDispose: false,
        dependencies: null,
        $allTransitiveDependencies: null,
      );

  @override
  String debugGetCreateSourceHash() => _$wizardHash();

  @$internal
  @override
  Wizard create() => Wizard();

  /// {@macro riverpod.override_with_value}
  Override overrideWithValue(WizardPhase value) {
    return $ProviderOverride(
      origin: this,
      providerOverride: $SyncValueProvider<WizardPhase>(value),
    );
  }
}

String _$wizardHash() => r'02b632ec728b6e17d054b30c11ba12cc96125db6';

/// The wizard controller (spec/frontend.md §6, §9).
///
/// Owns the incremental image index (the GUI's single source of truth for
/// image metadata, §5) and the linear phase machine. One pass stream is in
/// flight at a time; `EnsureModel` is exempt (it runs in [ModelStatus]).

abstract class _$Wizard extends $Notifier<WizardPhase> {
  WizardPhase build();
  @$mustCallSuper
  @override
  WhenComplete runBuild() {
    final ref = this.ref as $Ref<WizardPhase, WizardPhase>;
    final element =
        ref.element
            as $ClassProviderElement<
              AnyNotifier<WizardPhase, WizardPhase>,
              WizardPhase,
              Object?,
              Object?
            >;
    return element.handleCreate(ref, build);
  }
}
