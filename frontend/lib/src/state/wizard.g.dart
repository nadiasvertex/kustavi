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
final class WizardProvider extends $AsyncNotifierProvider<Wizard, WizardPhase> {
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
}

String _$wizardHash() => r'bf76b915fb45506467eb4cf8f2ad21573de52acf';

/// The wizard controller (spec/frontend.md §6, §9).
///
/// Owns the incremental image index (the GUI's single source of truth for
/// image metadata, §5) and the linear phase machine. One pass stream is in
/// flight at a time; `EnsureModel` is exempt (it runs in [ModelStatus]).

abstract class _$Wizard extends $AsyncNotifier<WizardPhase> {
  FutureOr<WizardPhase> build();
  @$mustCallSuper
  @override
  WhenComplete runBuild() {
    final ref = this.ref as $Ref<AsyncValue<WizardPhase>, WizardPhase>;
    final element =
        ref.element
            as $ClassProviderElement<
              AnyNotifier<AsyncValue<WizardPhase>, WizardPhase>,
              AsyncValue<WizardPhase>,
              Object?,
              Object?
            >;
    return element.handleCreate(ref, build);
  }
}
