// sysagent: a hand-rolled, single-endpoint HTTP/1.1 server. No framework —
// same philosophy as the raw-socket chatterd examples earlier in the book.
#pragma once

namespace sysagent {

// Serve GET /metrics (JSON snapshot, see procfs.hpp) on 0.0.0.0:port until
// SIGINT/SIGTERM. Each request takes a fresh snapshot with the given
// interval_ms. Returns the process exit code (0 on a clean signal shutdown).
[[nodiscard]] int serve(int port, int interval_ms);

} // namespace sysagent
