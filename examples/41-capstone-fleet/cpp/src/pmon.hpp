// pmon.hpp — the fleet's init: drops the capability bounding set, then
// self-re-execs chatterd/sysagent/fwatch as supervised children. See
// pmon.cpp for the fork/exec + sigwait shutdown design.
#pragma once

#include <string>

namespace pmon {

int run(const std::string& node, const std::string& sandbox_dir, const std::string& peer,
        const std::string& peer_node, int chatterd_port, int health_interval_ms);

} // namespace pmon
