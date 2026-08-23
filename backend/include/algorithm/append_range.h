#pragma once

namespace kustavi::algorithm {
/** Appends `values` to a proto repeated field. `*Add() = value` is the
 * sanctioned copy path (RepeatedPtrField::Add(const T&) is deleted). */
template <class Field, class Range>
auto append_range(Field *field, const Range &values) -> void {
  for (const auto &value : values) {
    *field->Add() = value;
  }
}
} // namespace kustavi::algorithm
