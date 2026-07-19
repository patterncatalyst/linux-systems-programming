// sysagent v0 (chapter 36: /proc, /sys, and the agent) — a USE-method
// metrics collector reading /proc + /sys/fs/cgroup, no root required.
//
//   sysagent sample [--json] [--interval-ms N]
//   sysagent serve  --port P [--interval-ms N]
//
// `sample` takes one snapshot: CPU utilization from an /proc/stat delta
// spanning --interval-ms (default 200), run-queue + load from
// /proc/loadavg, memory from /proc/meminfo, per-disk I/O from
// /proc/diskstats, network rx/tx from /proc/net/dev, and cgroup PSI
// (falling back to system-wide /proc/pressure) if the kernel exposes it.
// `serve` exposes the identical snapshot as JSON over a hand-rolled
// HTTP/1.1 /metrics endpoint, one fresh sample per request.
//
// The field names in both the --json and /metrics output are the
// deterministic, cross-language schema documented in README.md — Go and
// Rust sysagent emit byte-for-byte the same keys.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>
#include <string>
#include <string_view>

#include "httpd.hpp"
#include "procfs.hpp"

namespace {

void usage() {
    std::println(stderr, "usage: sysagent sample [--json] [--interval-ms N] | "
                          "serve --port P [--interval-ms N]");
}

// Minimal hand-rolled flag scan — no getopt_long, to keep the three
// languages' argument handling directly comparable.
struct Flags {
    bool json = false;
    int interval_ms = 200;
    int port = -1;
};

bool parse_flags(int argc, char** argv, int start, Flags& out) {
    for (int i = start; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--json") {
            out.json = true;
        } else if (a == "--interval-ms") {
            if (i + 1 >= argc) return false;
            out.interval_ms = std::atoi(argv[++i]);
        } else if (a == "--port") {
            if (i + 1 >= argc) return false;
            out.port = std::atoi(argv[++i]);
        } else {
            return false;
        }
    }
    return true;
}

int cmd_sample(int argc, char** argv) {
    Flags flags;
    if (!parse_flags(argc, argv, 2, flags) || flags.interval_ms <= 0) {
        usage();
        return 2;
    }
    auto snap = sysagent::take_snapshot(flags.interval_ms);
    if (!snap) {
        std::println(stderr, "sysagent: error: {}", snap.error());
        return 1;
    }
    if (flags.json) {
        std::println("{}", sysagent::to_json(*snap));
    } else {
        std::print("{}", sysagent::to_text(*snap));
    }
    return 0;
}

int cmd_serve(int argc, char** argv) {
    Flags flags;
    if (!parse_flags(argc, argv, 2, flags) || flags.port <= 0 || flags.interval_ms <= 0) {
        usage();
        return 2;
    }
    return sysagent::serve(flags.port, flags.interval_ms);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    std::string_view cmd = argv[1];
    if (cmd == "sample") return cmd_sample(argc, argv);
    if (cmd == "serve") return cmd_serve(argc, argv);
    usage();
    return 2;
}
