// 41-capstone-fleet — pmon supervises chatterd + sysagent + fwatch across two
// lab hosts, capability-dropped and Landlock-sandboxed, all telemetry
// exported to the host LGTM stack. One binary, four subcommands (the same
// self-re-exec shape ch34's container entrypoint uses): pmon is the fleet's
// init; chatterd/sysagent/fwatch are the services it supervises.
//
// This file is a direct, line-for-line port of go/main.go's flag parsing
// (argFlag/hasFlag/firstPositional/atoiOr) and dispatch table — the usage
// banner text below is byte-identical to the Go reference's, since
// verify.lua and every language's demo.sh depend on it matching exactly.
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <print>
#include <string>
#include <utility>
#include <vector>

#include "chatterd.hpp"
#include "fwatch.hpp"
#include "pmon.hpp"
#include "sysagent.hpp"
#include "telemetry.hpp"

namespace {

void usage() {
    std::print(stderr,
                "usage: app <command>\n"
                "  pmon [--node NAME] [--sandbox-dir DIR] [--peer HOST:PORT] [--peer-node NAME]\n"
                "       [--chatterd-port P] [--health-interval-ms N]\n"
                "  chatterd serve [--host H] [--port P] [--node NAME] [--peer HOST:PORT] [--peer-node NAME]\n"
                "  chatterd send   --host H --port P --nick NICK --text TEXT [--timeout-ms T]\n"
                "  chatterd listen --host H --port P --nick NICK [--timeout-ms T]\n"
                "  sysagent [--node NAME] [--interval-ms N] [--once]\n"
                "  sysagent saturate --resource cpu|mem --seconds N [--workers K|--mb M]\n"
                "  fwatch snapshot DIR\n"
                "  fwatch watch DIR [--sandbox] [--timeout-ms T]\n");
}

// argFlag scans args for "--name value" and returns (value, true), or
// (def, true) if absent, or (_, false) if the flag is present but has no
// following value — mirrors go/main.go's argFlag exactly.
std::pair<std::string, bool> arg_flag(const std::vector<std::string>& args, const std::string& name,
                                       const std::string& def) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == name) {
            if (i + 1 < args.size()) {
                return {args[i + 1], true};
            }
            return {"", false};
        }
    }
    return {def, true};
}

bool has_flag(const std::vector<std::string>& args, const std::string& name) {
    for (const auto& a : args) {
        if (a == name) {
            return true;
        }
    }
    return false;
}

// firstPositional: walks args, treating every "--xxx" token as consuming
// exactly one following token (its value), and returns the first token that
// survives that skipping — mirrors go/main.go's firstPositional exactly,
// including its "skip the very next token no matter what it is" quirk.
std::string first_positional(const std::vector<std::string>& args) {
    bool skip = false;
    for (const auto& a : args) {
        if (skip) {
            skip = false;
            continue;
        }
        if (a.size() >= 2 && a[0] == '-' && a[1] == '-') {
            skip = true;
            continue;
        }
        return a;
    }
    return "";
}

int atoi_or(const std::string& s, int def) {
    if (s.empty()) {
        return def;
    }
    try {
        std::size_t pos = 0;
        const int v = std::stoi(s, &pos);
        if (pos != s.size()) {
            return def;
        }
        return v;
    } catch (...) {
        return def;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string cmd = argv[1];
    const std::vector<std::string> rest(argv + 2, argv + argc);

    if (cmd == "pmon") {
        auto [node, ok1] = arg_flag(rest, "--node", "node1");
        auto [sandbox_dir, ok2] = arg_flag(rest, "--sandbox-dir", "/tmp/fwatch-sandbox");
        auto [peer, ok3] = arg_flag(rest, "--peer", "");
        auto [peer_node, ok4] = arg_flag(rest, "--peer-node", "");
        auto [port_s, ok5] = arg_flag(rest, "--chatterd-port", "47100");
        auto [health_s, ok6] = arg_flag(rest, "--health-interval-ms", "2000");
        if (!(ok1 && ok2 && ok3 && ok4 && ok5 && ok6)) {
            usage();
            return 2;
        }
        return pmon::run(node, sandbox_dir, peer, peer_node, atoi_or(port_s, 47100), atoi_or(health_s, 2000));
    }

    if (cmd == "chatterd") {
        if (rest.empty()) {
            usage();
            return 2;
        }
        const std::vector<std::string> args(rest.begin() + 1, rest.end());
        if (rest[0] == "serve") {
            auto [host, _h] = arg_flag(args, "--host", "0.0.0.0");
            auto [port_s, _p] = arg_flag(args, "--port", "47100");
            auto [node, _n] = arg_flag(args, "--node", "node1");
            auto [peer, _pe] = arg_flag(args, "--peer", "");
            auto [peer_node, _pn] = arg_flag(args, "--peer-node", "");
            telemetry::Handle tel = telemetry::init("chatterd", node);
            const int code = chatterd::serve(host, atoi_or(port_s, 47100), node, peer, peer_node, tel);
            tel.shutdown();
            return code;
        }
        if (rest[0] == "send") {
            auto [host, _h] = arg_flag(args, "--host", "127.0.0.1");
            auto [port_s, _p] = arg_flag(args, "--port", "47100");
            auto [nick, ok1] = arg_flag(args, "--nick", "");
            auto [text, ok2] = arg_flag(args, "--text", "");
            auto [timeout_s, _t] = arg_flag(args, "--timeout-ms", "3000");
            if (!ok1 || !ok2 || nick.empty() || text.empty()) {
                usage();
                return 2;
            }
            return chatterd::send(host, atoi_or(port_s, 47100), nick, text, atoi_or(timeout_s, 3000));
        }
        if (rest[0] == "listen") {
            auto [host, _h] = arg_flag(args, "--host", "127.0.0.1");
            auto [port_s, _p] = arg_flag(args, "--port", "47100");
            auto [nick, ok1] = arg_flag(args, "--nick", "");
            auto [timeout_s, _t] = arg_flag(args, "--timeout-ms", "5000");
            if (!ok1 || nick.empty()) {
                usage();
                return 2;
            }
            return chatterd::listen(host, atoi_or(port_s, 47100), nick, atoi_or(timeout_s, 5000));
        }
        usage();
        return 2;
    }

    if (cmd == "sysagent") {
        if (!rest.empty() && rest[0] == "saturate") {
            const std::vector<std::string> args(rest.begin() + 1, rest.end());
            auto [resource, _r] = arg_flag(args, "--resource", "");
            auto [seconds_s, _s] = arg_flag(args, "--seconds", "10");
            auto [workers_s, _w] = arg_flag(args, "--workers", "0");
            auto [mb_s, _m] = arg_flag(args, "--mb", "0");
            return sysagent::saturate(resource, atoi_or(seconds_s, 10), atoi_or(workers_s, 0), atoi_or(mb_s, 0));
        }
        auto [node, _n] = arg_flag(rest, "--node", "node1");
        auto [interval_s, _i] = arg_flag(rest, "--interval-ms", "2000");
        const bool once = has_flag(rest, "--once");
        telemetry::Handle tel = telemetry::init("sysagent", node);
        const int code = sysagent::run(node, atoi_or(interval_s, 2000), once, tel);
        tel.shutdown();
        return code;
    }

    if (cmd == "fwatch") {
        if (rest.empty()) {
            usage();
            return 2;
        }
        const std::vector<std::string> args(rest.begin() + 1, rest.end());
        if (rest[0] == "snapshot") {
            const std::string dir = first_positional(args);
            if (dir.empty()) {
                usage();
                return 2;
            }
            return fwatch::snapshot(dir);
        }
        if (rest[0] == "watch") {
            const std::string dir = first_positional(args);
            if (dir.empty()) {
                usage();
                return 2;
            }
            const bool sandbox = has_flag(args, "--sandbox");
            auto [timeout_s, _t] = arg_flag(args, "--timeout-ms", "0");
            return fwatch::watch(dir, sandbox, atoi_or(timeout_s, 0));
        }
        usage();
        return 2;
    }

    usage();
    return 2;
}
