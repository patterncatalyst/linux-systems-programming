// senders.cpp -- the seventh arm, and the one ch49 promised conditionally.
//
// ch49 measured that GCC's standard library has no P2300 (__cpp_lib_senders
// undefined, <execution> still the C++17 parallel-algorithms header) and said
// the live comparison went into ch56 "if and only if the toolchain has caught
// up". It has NOT: this arm links the NVIDIA stdexec REFERENCE IMPLEMENTATION,
// pinned by the Conan sub-target, and that distinction is load-bearing
// everywhere it is reported.
//
// It runs the identical workload from workload.hpp, folding into one shared
// accumulator under a mutex. That is deliberately NOT idiomatic sender code --
// the idiom would compose per-task values and never share state -- but a
// different workload would make the comparison meaningless, and being able to
// say why is worth more here than being idiomatic.

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include "workload.hpp"

using namespace lsp56;

int main(int argc, char** argv) {
    if (argc != 2 || std::string(argv[1]) != "senders") {
        std::fprintf(stderr, "usage: capstone-senders senders\n");
        return 2;
    }

    std::uint64_t acc = 0;
    std::mutex m;

    exec::static_thread_pool pool(kTasks);
    auto sched = pool.get_scheduler();

    // One sender per task, each describing the same work as a VALUE, then all
    // of them started at once and waited on. stdexec::on places the work on the
    // pool's scheduler -- "what work, on what execution resource", which is the
    // concurrency/parallelism split of ch49 expressed in the type system.
    auto fold_one = [&](int task) {
        return stdexec::on(sched, stdexec::just(task) | stdexec::then([&](int t) {
                               note_tid_once();
                               for (int r = 0; r < kRounds; ++r) {
                                   const std::lock_guard<std::mutex> lock(m);
                                   acc += unit(t, r);
                               }
                               return 0;
                           }));
    };

    stdexec::sync_wait(stdexec::when_all(fold_one(0), fold_one(1), fold_one(2), fold_one(3),
                                         fold_one(4), fold_one(5), fold_one(6), fold_one(7)));

    report_arm("senders", acc);
    std::printf("senders: implementation=NVIDIA-stdexec (reference), stdlib_p2300=no\n");
    return 0;
}
