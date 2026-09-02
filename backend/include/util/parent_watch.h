#pragma once

#include <cstdint>

namespace kustavi::util {

/** Force this process to exit as soon as the launching GUI is gone.
 *
 * Spawns one detached background thread that watches the process identified
 * by `parent_pid`. If that process disappears for any reason the `Shutdown`
 * RPC does not cover -- window close, a crash, `SIGKILL`, `flutter run`
 * being Ctrl-C'd -- the watcher calls `std::_Exit` so no orphaned back end
 * is left running (spec/frontend.md §3.2).
 *
 * A `parent_pid` of 0 (or negative) disables the watcher; that is the
 * default for development runs started without the GUI.
 */
void watch_parent_and_exit(std::int64_t parent_pid);

} // namespace kustavi::util
