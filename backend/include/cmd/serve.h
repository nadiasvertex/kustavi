#pragma once

#include <cstdint>
#include <string>

namespace kustavi::cmd {

/** Serve the gRPC API on the given loopback endpoint.
 *
 * Blocks until the server is shut down (via the `Shutdown` RPC) or the
 * process is terminated.
 *
 * @param host The loopback host to bind to (127.0.0.1 or ::1).
 * @param port The port to bind to (0 = OS-assigned).
 * @param auth_token The token validated on every incoming gRPC call. When
 * empty, validation is skipped (development mode).
 * @param parent_pid PID of the launching GUI. When positive, the back end
 * forces itself to exit as soon as that process is gone so it is never
 * orphaned (spec/frontend.md §3.2). 0 disables the watch.
 */
void serve(const std::string &host, int port, const std::string &auth_token,
           std::int64_t parent_pid = 0);
} // namespace kustavi::cmd
