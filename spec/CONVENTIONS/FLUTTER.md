# Dart & Flutter LLM-Optimized Code Conventions 

This document outlines the strict coding standards and architectural principles for Dart and Flutter development. 

---

## 1. System Persona & Core Rules
* **Role**: You are an expert Dart 3.x and Flutter 3.x software architect prioritizing functional design, type safety, and runtime predictability.
* **Production-Ready Code**: Never emit placeholder code, incomplete structures, or `// TODO` comments. Write fully functional, compilation-ready code blocks.
* **No Code-Bloat**: Write self-documenting code. Keep comments minimal, focusing only on explaining complex algorithmic choices or business domain invariants.

---

## 2. Recommended Stack Foundations
* **State Management**: **Riverpod (v2.x+)** using the code-generation syntax (`@riverpod`). 
  * Avoid raw `ChangeNotifier` or imperative `setState` blocks for global or feature state.
  * Use functional providers for synchronous operations and `AsyncNotifier` for asynchronous, side-effect-driven states.
* **Networking & RPC**: **gRPC (via standard protocol buffers)**.
  * Prioritize generated client stubs and domain-mapped models over raw JSON parsing.
* **Linting / Code Quality**: **`flutter_lints` with strict static analysis extensions**.
  * Enforce strict raw types (`strict-casts: true`, `strict-inference: true`, `strict-raw-types: true`).

---

## 3. Latest Dart 3 Features & Idioms
Always leverage modern Dart idioms to achieve a declarative, functional syntax.

### 3.1. Sealed Classes & Domain Modeling
Model state variations, domain errors, and data envelopes using `sealed` class hierarchies to leverage exhaustive compiler checking.

```dart
sealed class FetchState<T> {}

class Initial<T> extends FetchState<T> {}
class Loading<T> extends FetchState<T> {}
class Success<T> extends FetchState<T> {
  final T data;
  const Success(this.data);
}
class Failure<T> extends FetchState<T> {
  final String message;
  const Failure(this.message);
}
```

### 3.2. Pattern Matching & Switch Expressions
Prefer inline switch expressions over imperative if-else chains or standard switch statements.

```dart
Widget buildStateView(FetchState<String> state) {
  return switch (state) {
    Initial() => const Text('Initialize action...'),
    Loading() => const CircularProgressIndicator(),
    Success(:final data) => Text('Loaded: $data'),
    Failure(:final message) => Text('Error: $message'),
  };
}
```

### 3.3. Records & Destructuring
Use records for quick structural data aggregation and multiple return values, replacing single-purpose temporary wrapper classes.

```dart
// Explicit record naming for readability
(double latitude, double longitude) getCoordinates() {
  return (60.1699, 24.9384);
}

void useCoordinates() {
  final (lat, lng) = getCoordinates();
  print('Lat: $lat, Lng: $lng');
}
```

---

## 4. Functional Style & Immutability
Minimize mutable structures, explicit loop variables, and side-effects.

* **Immutable Collections**: Prefer deep immutability. Use `List.unmodifiable`, or map transformations directly to final fields.
* **Functional Operators**: Use iterable higher-order methods (`.map()`, `.where()`, `.fold()`, `.take()`) rather than imperative `for` and `while` loops.
* **Pure Functions**: Write deterministic functions that accept arguments and return outputs without mutating ambient or global scopes.

### 4.1. Functional Error Handling (Errors as Values)
Do not use `throw` for expected operational failures (e.g., network timeout, gRPC unauthenticated errors). Return errors explicitly as structural records.

```dart
// Success and Error represented as a record value
typedef Result<T> = (T? value, Exception? error);

Future<Result<String>> fetchUserData(UserClient client, String userId) async {
  try {
    final response = await client.getUser(UserRequest()..id = userId);
    return (response.name, null);
  } on GrpcError catch (e) {
    return (null, e);
  }
}
```

---

## 5. Flutter Architecture & Widget Layout
* **Declarative Layouts**: Maintain a strict separation between UI rendering and business logic.
* **Prefer Const Constructors**: Every widget that can be instantiated with `const` must be declared as `const` to optimize Flutter engine repaint mechanics.
* **Widgets vs Micro-functions**: Do not split massive UI code into small helper methods like `Widget _buildRow()`. Use explicit, decoupled `const StatelessWidget` classes. This ensures local widget tree rebuilding isolation.

```dart
// BAD: Causes global rebuild penalties
Widget _buildProfileHeader() {
  return Row(children: [const Text('User Header')]);
}

// GOOD: Fully isolated, optimization-ready
class ProfileHeader extends StatelessWidget {
  const ProfileHeader({super.key});

  @override
  Widget build(BuildContext context) {
    return const Row(
      children: [Text('User Header')],
    );
  }
}
```

---

## 6. gRPC & Asynchronous Integration with Riverpod
Ensure that gRPC channels, clients, and stream lifecycles are safely managed and automatically disposed of by the Riverpod graph.

```dart
import 'package:grpc/grpc.dart';
import 'package:riverpod_annotation/riverpod_annotation.dart';

part 'provider.g.dart';

@riverpod
ClientChannel grpcChannel(GrpcChannelRef ref) {
  final channel = ClientChannel(
    'api.example.com',
    port: 50051,
    options: const ChannelOptions(credentials: ChannelCredentials.insecure()),
  );
  
  // Clean up structural networking resources when provider closes
  ref.onDispose(() async {
    await channel.shutdown();
  });
  
  return channel;
}

@riverpod
UserClient userClient(UserClientRef ref) {
  final channel = ref.watch(grpcChannelProvider);
  return UserClient(channel);
}
```

---

## 7. Configuration Reference for LLM Code generation
When asked to write `analysis_options.yaml`, always emit this precise block to force strict checking constraints:

```yaml
analyzer:
  language:
    strict-casts: true
    strict-inference: true
    strict-raw-types: true
  errors:
    missing_required_param: error
    missing_return: error
    todo: ignore

linter:
  rules:
    - always_declare_return_types
    - avoid_empty_else
    - avoid_relative_lib_imports
    - avoid_shadowing_type_parameters
    - avoid_types_as_parameter_names
    - await_only_futures
    - camel_case_extensions
    - camel_case_types
    - cancel_subscriptions
    - close_sinks
    - curly_braces_in_flow_control_structures
    - empty_catches
    - file_names
    - hash_and_equals
    - no_duplicate_case_values
    - non_constant_identifier_names
    - prefer_conditional_assignment
    - prefer_const_constructors
    - prefer_const_declarations
    - prefer_is_empty
    - prefer_is_not_empty
    - prefer_iterable_whereType
    - prefer_single_quotes
    - unawaited_futures
    - unnecessary_const
    - unnecessary_new
    - unnecessary_null_aware_assignments
    - unnecessary_nullable_for_final_variable_declarations
    - unnecessary_string_interpolations
    - use_build_context_synchronously
    - valid_regexps
```
