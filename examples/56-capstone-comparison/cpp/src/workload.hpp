// workload.hpp -- ONE workload, shared verbatim by every arm of ch56.
//
// This header exists so that the comparison is a comparison. Each of ch50-ch55
// measured its own model, with its own instrument, on its own workload, so no
// two numbers in the compendium are strictly comparable. Here every model runs
// THIS function, kTasks * kRounds times, and folds into ONE accumulator.
//
// The fold is addition on uint64, which is commutative and associative, so the
// accumulator does not depend on the order the models happen to run in -- and
// the models genuinely differ in that order. A model that produced a different
// total would not be a slower answer to the same question, it would be a wrong
// one, which is why gate B checks the total before any comparison is drawn.

#ifndef LSP_CH56_WORKLOAD_HPP
#define LSP_CH56_WORKLOAD_HPP

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <set>

#include <unistd.h>

namespace lsp56 {

// ch46-ch55's payload ("The quick brown."), carried forward unchanged.
inline constexpr std::uint8_t kPayload[16] = {0x54, 0x68, 0x65, 0x20, 0x71, 0x75, 0x69, 0x63,
                                              0x6b, 0x20, 0x62, 0x72, 0x6f, 0x77, 0x6e, 0x2e};

inline constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
inline constexpr std::uint64_t kFnvPrime = 0x00000100000001b3ULL;

// 8 tasks x 5000 rounds = 40000 folds into the shared accumulator. The scale is
// chosen so the contended arms reach the futex the way ch51's 200,000
// increments did, while the arm that posts one handler per fold (asio) stays
// comfortably inside its timeout.
inline constexpr int kTasks = 8;
inline constexpr int kRounds = 5000;

inline std::uint64_t fnv1a(const std::uint8_t* data, std::size_t len, std::uint64_t seed) {
    std::uint64_t h = seed;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= kFnvPrime;  // uint64_t wraps by definition -- no UB to sanitize
    }
    return h;
}

// The unit of work. Deterministic in (task, round) and independent of every
// other unit, so any interleaving any model produces folds to the same total.
inline std::uint64_t unit(int task, int round) {
    const std::uint64_t seed =
        kFnvOffsetBasis ^ (static_cast<std::uint64_t>(task) * 1000003ULL +
                           static_cast<std::uint64_t>(round));
    return fnv1a(kPayload, sizeof kPayload, seed);
}

// What every arm must produce, computed the dullest possible way.
inline std::uint64_t expected_total() {
    std::uint64_t acc = 0;
    for (int t = 0; t < kTasks; ++t) {
        for (int r = 0; r < kRounds; ++r) {
            acc += unit(t, r);
        }
    }
    return acc;
}

// ── instrument 1: who ran it (ch50's gettid, ch49's vocabulary) ───────────
//
// Recorded ONCE per thread rather than once per fold. That matters: this set
// is guarded by a mutex, and a mutex touched 40000 times would land in the
// futex counts that instrument 2 is trying to measure. Eight acquisitions is
// noise against 40000 folds; 40000 would have been the measurement.

inline std::mutex g_tid_mutex;
inline std::set<long> g_tids;

inline void note_tid_once() {
    thread_local bool noted = false;
    if (noted) {
        return;
    }
    noted = true;
    const std::lock_guard<std::mutex> lock(g_tid_mutex);
    g_tids.insert(static_cast<long>(gettid()));
}

// ── reporting ────────────────────────────────────────────────────────────
//
// Instrument 2 (futex calls) is counted from OUTSIDE the process with
// strace -f -c -e trace=futex, exactly as ch51 and ch55 counted it. Nothing
// in this program measures it, and nothing in this program is timed (ch39).

inline void report_arm(const char* model, std::uint64_t total) {
    const std::uint64_t want = expected_total();
    std::printf("%s: tasks=%d rounds=%d folds=%d total=0x%016llx correct=%s\n", model, kTasks,
                kRounds, kTasks * kRounds, static_cast<unsigned long long>(total),
                total == want ? "yes" : "no");
    std::printf("%s: distinct_tids=%zu\n", model, g_tids.size());
    std::printf("%s report: case=%s digest=0x%016llx\n", model, model,
                static_cast<unsigned long long>(fnv1a(kPayload, sizeof kPayload, kFnvOffsetBasis)));
}

}  // namespace lsp56

#endif  // LSP_CH56_WORKLOAD_HPP
