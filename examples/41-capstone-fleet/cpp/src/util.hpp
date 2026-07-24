// util.hpp — small helpers shared across the fleet's subcommands, the C++
// counterpart of util.go's installSignalFlag: chatterd/sysagent/fwatch each
// run their own poll loop already (accept-with-timeout, interval sleep,
// poll(2) with a timeout), so a level-triggered flag is all any of them
// needs from a signal — no EINTR-sensitive syscall unwinding required, unlike
// ex40's fastpath server. pmon (a supervisor, not a poller) does its own
// dedicated sigwait() handling in pmon.cpp instead of using this.
#pragma once

#include <atomic>

namespace util {

// Installs a SIGINT/SIGTERM handler (once per process; safe to call from
// multiple subcommands' code paths) that flips a process-wide flag to true
// the first time either signal arrives. Returns a reference to that flag.
std::atomic<bool>& install_signal_flag();

} // namespace util
