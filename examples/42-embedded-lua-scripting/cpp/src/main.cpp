// pmon — chapter 42: an embedded-Lua restart policy via sol2.
//
// The restart/backoff decision lives entirely in policy.lua, not in this
// binary: the host loads it into a *sandboxed* Lua 5.4 state (base/string/
// table/math only — no os, io, package, debug; load/loadfile/dofile/require/
// collectgarbage/loadstring removed even though their libraries were never
// opened, since a global with that name could otherwise be reintroduced by a
// careless policy), calls `on_exit(info)` for every simulated child exit, and
// prints exactly what the policy hands back. A real SIGHUP — blocked with
// sigprocmask and consumed as data through a signalfd + poll, the same
// pattern ch12/ch13 use for SIGCHLD — reloads the policy file with no
// restart of pmon itself: the running supervisor keeps its child-tracking
// state, only the decision logic changes underneath it.

#include <sol/sol.hpp>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <expected>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

namespace {

using std::chrono::steady_clock;

[[nodiscard]] std::error_code last_error() {
    return {errno, std::system_category()};
}

// Every stdout write is a bare fflush away from a race with the parent shell
// that greps this process's redirected output file for a sentinel line (see
// demo.sh's `run()`) — std::println is fully buffered once stdout is not a
// tty, so every print here is followed by an explicit flush.
void say(std::string_view line) {
    std::println("{}", line);
    std::fflush(stdout);
}

[[noreturn]] void die(const std::string& msg) {
    std::println(stderr, "pmon: {}", msg);
    std::exit(1);
}

// ---------------------------------------------------------------------------
// Sandbox: open only base/string/table/math, then strip the globals that
// would let a policy touch the filesystem, spawn processes, or load new code
// even though their owning libraries (os, io, package, debug) were never
// opened in the first place.
// ---------------------------------------------------------------------------

sol::state make_sandbox() {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math);
    lua["load"] = sol::lua_nil;
    lua["loadfile"] = sol::lua_nil;
    lua["dofile"] = sol::lua_nil;
    lua["require"] = sol::lua_nil;
    lua["collectgarbage"] = sol::lua_nil;
    lua["loadstring"] = sol::lua_nil;

    sol::table host = lua.create_named_table("host");
    host.set_function("log", [](const std::string& msg) { say("pmon: policy: " + msg); });
    host.set_function("now_ms", []() -> long long {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   steady_clock::now().time_since_epoch())
            .count();
    });
    return lua;
}

// ---------------------------------------------------------------------------
// Errors, three ways (C++): fallible operations return std::expected<T,
// std::string>, converting a failed sol::protected_function_result into its
// sol::error message; die() is reserved for CLI-usage/OS-resource failures
// that have no data-driven recovery.
// ---------------------------------------------------------------------------

// Load `path` under a protected parse+run, then call on_load() if the policy
// defines one (also protected). Any failure here is a policy-load error.
[[nodiscard]] std::expected<void, std::string> load_policy(sol::state& lua, const std::string& path) {
    sol::protected_function_result loaded = lua.safe_script_file(path, sol::script_pass_on_error);
    if (!loaded.valid()) {
        sol::error err = loaded;
        return std::unexpected(err.what());
    }
    sol::protected_function on_load = lua["on_load"];
    if (on_load.valid()) {
        sol::protected_function_result ran = on_load();
        if (!ran.valid()) {
            sol::error err = ran;
            return std::unexpected(err.what());
        }
    }
    return {};
}

// One simulated child-exit event to feed on_exit().
struct Event {
    std::string_view name;
    long long exit_code;
    long long restarts;
    long long consecutive_failures;
    long long uptime_ms;
    long long last_backoff_ms;
};

struct Decision {
    std::string action;
    long long delay_ms = 0;
    std::string reason;
};

// Build `info`, call the policy's on_exit(info) under a protected call, and
// read back action/delay_ms/reason. `signal` stays absent from the table
// (nil) for every event this chapter replays.
[[nodiscard]] std::expected<Decision, std::string> decide(sol::state& lua, const Event& ev) {
    sol::table info = lua.create_table();
    info["name"] = std::string{ev.name};
    info["exit_code"] = ev.exit_code;
    info["restarts"] = ev.restarts;
    info["consecutive_failures"] = ev.consecutive_failures;
    info["uptime_ms"] = ev.uptime_ms;
    info["last_backoff_ms"] = ev.last_backoff_ms;

    sol::protected_function fn = lua["on_exit"];
    if (!fn.valid()) {
        return std::unexpected(std::string{"policy defines no on_exit"});
    }
    sol::protected_function_result result = fn(info);
    if (!result.valid()) {
        sol::error err = result;
        return std::unexpected(err.what());
    }
    sol::table decision = result;
    return Decision{
        .action = decision["action"],
        .delay_ms = decision["delay_ms"],
        .reason = decision["reason"],
    };
}

void print_decision(std::string_view name, const Decision& dec) {
    say(std::format(R"(pmon: decision child={} action={} delay_ms={} reason="{}")", name, dec.action,
                     dec.delay_ms, dec.reason));
}

// ---------------------------------------------------------------------------
// SIGHUP, the systems-programming content: block it process-wide up front so
// there is never a window where it could interrupt a syscall or fire a
// default handler, then consume it as ordinary readability on a signalfd —
// same pattern ch12/ch13 use for SIGCHLD, just a different signal.
// ---------------------------------------------------------------------------

[[nodiscard]] std::expected<int, std::error_code> make_sighup_fd() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGHUP);
    if (::sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) {
        return std::unexpected(last_error());
    }
    const int fd = ::signalfd(-1, &mask, SFD_CLOEXEC);
    if (fd < 0) {
        return std::unexpected(last_error());
    }
    return fd;
}

// Block until SIGHUP arrives on `sigfd`, draining exactly one signalfd_siginfo.
void wait_for_sighup(int sigfd) {
    pollfd pfd{.fd = sigfd, .events = POLLIN, .revents = 0};
    for (;;) {
        const int n = ::poll(&pfd, 1, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            die("poll: " + last_error().message());
        }
        signalfd_siginfo info{};
        if (::read(sigfd, &info, sizeof info) == sizeof info && info.ssi_signo == SIGHUP) {
            return;
        }
    }
}

// Fresh sandbox + host table + protected load + on_load(), all rebuilt from
// scratch — the simplest correct way to guarantee reload leaves no state
// from the previous policy behind.
[[nodiscard]] std::expected<sol::state, std::string> reload(const std::string& policy_path) {
    sol::state lua = make_sandbox();
    if (auto loaded = load_policy(lua, policy_path); !loaded) {
        return std::unexpected(loaded.error());
    }
    return lua;
}

// ---------------------------------------------------------------------------
// Subcommands
// ---------------------------------------------------------------------------

int run_phase(sol::state& lua, const std::vector<Event>& events) {
    for (const auto& ev : events) {
        auto dec = decide(lua, ev);
        if (!dec) {
            std::println(stderr, "pmon: policy error: {}", dec.error());
            return 1;
        }
        print_decision(ev.name, *dec);
    }
    return 0;
}

int cmd_run(const std::string& policy_path) {
    auto sigfd = make_sighup_fd();
    if (!sigfd) {
        std::println(stderr, "pmon: signalfd: {}", sigfd.error().message());
        return 1;
    }

    auto phase1 = reload(policy_path);
    if (!phase1) {
        std::println(stderr, "pmon: policy error: {}", phase1.error());
        return 1;
    }
    sol::state lua = std::move(*phase1);

    static const std::vector<Event> kPhase1{
        {.name = "web", .exit_code = 0, .restarts = 0, .consecutive_failures = 0, .uptime_ms = 120000, .last_backoff_ms = 0},
        {.name = "worker", .exit_code = 1, .restarts = 0, .consecutive_failures = 0, .uptime_ms = 9000, .last_backoff_ms = 0},
        {.name = "worker", .exit_code = 1, .restarts = 1, .consecutive_failures = 1, .uptime_ms = 9000, .last_backoff_ms = 500},
    };
    if (run_phase(lua, kPhase1) != 0) {
        return 1;
    }

    say("pmon: awaiting SIGHUP to reload policy");
    wait_for_sighup(*sigfd);
    say("pmon: reload requested");
    ::close(*sigfd);

    // Re-read the same --policy path — its content is now policy-v2.lua.
    auto phase2 = reload(policy_path);
    if (!phase2) {
        std::println(stderr, "pmon: policy error: {}", phase2.error());
        return 1;
    }
    lua = std::move(*phase2);

    static const std::vector<Event> kPhase2{
        {.name = "api", .exit_code = 1, .restarts = 0, .consecutive_failures = 0, .uptime_ms = 5000, .last_backoff_ms = 0},
        {.name = "worker", .exit_code = 1, .restarts = 2, .consecutive_failures = 2, .uptime_ms = 9000, .last_backoff_ms = 1000},
    };
    if (run_phase(lua, kPhase2) != 0) {
        return 1;
    }
    return 0;
}

int cmd_check(const std::string& policy_path) {
    sol::state lua = make_sandbox();
    if (auto loaded = load_policy(lua, policy_path); !loaded) {
        std::println("pmon: policy error: {}", loaded.error());
        std::fflush(stdout);
        return 1;
    }

    const Event probe{.name = "probe", .exit_code = 1, .restarts = 0, .consecutive_failures = 0,
                       .uptime_ms = 9000, .last_backoff_ms = 0};
    auto dec = decide(lua, probe);
    if (!dec) {
        std::println("pmon: policy error: {}", dec.error());
        std::fflush(stdout);
        return 1;
    }

    std::string version = lua["POLICY_VERSION"];
    say(std::format("pmon: policy ok version={}", version));
    return 0;
}

void usage() {
    std::println(stderr, "usage: app <run|check> --policy PATH");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string_view subcommand = argv[1];
    if (subcommand != "run" && subcommand != "check") {
        usage();
        return 2;
    }

    std::optional<std::string> policy;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--policy" && i + 1 < argc) {
            policy = argv[++i];
        } else {
            usage();
            return 2;
        }
    }
    if (!policy) {
        usage();
        return 2;
    }

    return subcommand == "run" ? cmd_run(*policy) : cmd_check(*policy);
}
