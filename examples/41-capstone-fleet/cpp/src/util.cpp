#include "util.hpp"

#include <csignal>
#include <mutex>

namespace util {

namespace {
std::atomic<bool> g_signal_flag{false};
std::once_flag g_installed;

extern "C" void on_signal(int) { g_signal_flag.store(true, std::memory_order_relaxed); }
} // namespace

std::atomic<bool>& install_signal_flag() {
    std::call_once(g_installed, [] {
        struct sigaction sa {};
        sa.sa_handler = on_signal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0; // level-triggered polling, not EINTR-driven: SA_RESTART doesn't matter here
        ::sigaction(SIGINT, &sa, nullptr);
        ::sigaction(SIGTERM, &sa, nullptr);
    });
    return g_signal_flag;
}

} // namespace util
