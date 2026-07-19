// app — bugfarm scenario 1: a crashing pmon-like supervisor
// (chapter 28: gdb and remote debugging).
//
//   app run     -- normal operation: reap a child, report its exit, exit 0
//   app crash   -- the SAME call path, fed a child record whose exit-status
//                  slot was never populated (as if handle_child() ran before
//                  the reaper filled it in) -> a null pointer dereference
//                  three frames down:
//
//                    main -> run_supervisor -> handle_child -> deref
//
// This binary is deliberately dual-use: a normal program in "run" mode, a
// debugging TARGET in "crash" mode. It is built with debug info (the
// "release" CMake preset is RelWithDebInfo = -O2 -g) so gdb can name every
// frame even though this is not a debug build; each frame function is
// marked noinline so -O2 cannot fold the call chain away.
//
// One more thing to notice once you're inside gdb: ShutdownGuard below never
// gets to print its message on the crash path. SIGSEGV is not a C++
// exception -- the kernel delivers a signal, the default disposition
// terminates the process immediately, and nothing unwinds the stack. RAII
// buys you cleanup on every *C++-level* exit path (return, throw, exit via
// exceptions); it buys you nothing against a hardware fault.

#include <cstdio>
#include <cstdlib>
#include <expected>
#include <print>
#include <string>
#include <string_view>

namespace {

// Runs at scope exit only if control leaves run_supervisor the ordinary way.
class ShutdownGuard {
  public:
    ~ShutdownGuard() { std::println("pmon: supervisor exiting cleanly"); }
};

// A child the supervisor is reporting on. exit_slot is filled in by the
// reaper once the wait status is known; handle_child reads through it to
// print the status line.
struct ChildRecord {
    int pid = 0;
    const int* exit_slot = nullptr;
};

[[gnu::noinline]] void deref(const ChildRecord& child) {
    // BUG: no null check before the read. In "crash" mode exit_slot is
    // nullptr because handle_child ran before the reaper recorded the exit
    // status -- exactly the kind of race a real supervisor can hit.
    std::println("pmon: child {} exited status={}", child.pid,
                 *child.exit_slot);
}

[[gnu::noinline]] void handle_child(const ChildRecord& child) { deref(child); }

[[gnu::noinline]] int run_supervisor(bool inject_bug) {
    std::println("pmon: supervisor starting");
    const int reaped_status = 0;
    ChildRecord child{.pid = 4242,
                      .exit_slot = inject_bug ? nullptr : &reaped_status};

    if (inject_bug) {
        handle_child(child);  // crash path: exit_slot stays null
        return 0;              // unreachable
    }
    ShutdownGuard guard;  // only ever constructed on the clean path
    handle_child(child);
    return 0;
}

enum class Mode { kRun, kCrash };

[[nodiscard]] std::expected<Mode, std::string>
parse_mode(std::string_view sub) {
    if (sub == "run") {
        return Mode::kRun;
    }
    if (sub == "crash") {
        return Mode::kCrash;
    }
    return std::unexpected("unknown subcommand: " + std::string(sub));
}

[[noreturn]] void usage() {
    std::println(stderr, "usage: app run");
    std::println(stderr, "       app crash");
    std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // line-buffered even into a file

    if (argc != 2) {
        usage();
    }
    const auto mode = parse_mode(argv[1]);
    if (!mode) {
        usage();
    }
    return run_supervisor(*mode == Mode::kCrash);
}
