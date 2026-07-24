// sysagent.cpp — /proc/stat, /proc/meminfo, /proc/loadavg readers and the
// sample loop, ported field-for-field from go/sysagent.go so the printed
// cpu_pct/mem_pct/load1 numbers are computed by the identical formula (not
// just "close enough"): idle is specifically the 4th space-separated value
// on /proc/stat's "cpu " line, and total is the sum of every value on that
// line including guest/guest_nice, exactly as the Go reference sums them.
#include "sysagent.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <expected>
#include <fstream>
#include <print>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <opentelemetry/metrics/observer_result.h>
#include <opentelemetry/nostd/variant.h>

#include "util.hpp"

namespace sysagent {

namespace {

struct CpuTimes {
    std::uint64_t idle = 0;
    std::uint64_t total = 0;
};

std::expected<CpuTimes, std::string> read_cpu_times() {
    std::ifstream f("/proc/stat");
    if (!f) {
        return std::unexpected("open /proc/stat failed");
    }
    std::string line;
    if (!std::getline(f, line)) {
        return std::unexpected("empty /proc/stat");
    }
    std::istringstream iss(line);
    std::string label;
    iss >> label;
    if (label != "cpu") {
        return std::unexpected("unexpected /proc/stat format: " + line);
    }
    CpuTimes t;
    std::uint64_t v = 0;
    int i = 0;
    while (iss >> v) {
        t.total += v;
        if (i == 3) { // idle is the 4th value after "cpu"
            t.idle = v;
        }
        ++i;
    }
    return t;
}

double cpu_pct(const CpuTimes& prev, const CpuTimes& cur) {
    const double d_total = static_cast<double>(cur.total - prev.total);
    const double d_idle = static_cast<double>(cur.idle - prev.idle);
    if (d_total <= 0) {
        return 0;
    }
    return (1.0 - d_idle / d_total) * 100.0;
}

std::expected<double, std::string> read_mem_pct() {
    std::ifstream f("/proc/meminfo");
    if (!f) {
        return std::unexpected("open /proc/meminfo failed");
    }
    double total = 0;
    double avail = 0;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream iss(line);
        std::string key;
        double v = 0;
        iss >> key >> v;
        if (key == "MemTotal:") {
            total = v;
        } else if (key == "MemAvailable:") {
            avail = v;
        }
    }
    if (total <= 0) {
        return std::unexpected("MemTotal not found");
    }
    return (1.0 - avail / total) * 100.0;
}

std::expected<double, std::string> read_load1() {
    std::ifstream f("/proc/loadavg");
    if (!f) {
        return std::unexpected("open /proc/loadavg failed");
    }
    double v = 0;
    f >> v;
    if (!f) {
        return std::unexpected("empty /proc/loadavg");
    }
    return v;
}

struct Sample {
    double cpu_pct = 0;
    double mem_pct = 0;
    double load1 = 0;
};

std::expected<Sample, std::string> take_sample(CpuTimes& prev) {
    auto cur = read_cpu_times();
    if (!cur) {
        return std::unexpected(cur.error());
    }
    Sample s;
    if (prev.total > 0) {
        s.cpu_pct = cpu_pct(prev, *cur);
    }
    prev = *cur;
    auto mp = read_mem_pct();
    if (!mp) {
        return std::unexpected(mp.error());
    }
    s.mem_pct = *mp;
    auto l1 = read_load1();
    if (!l1) {
        return std::unexpected(l1.error());
    }
    s.load1 = *l1;
    return s;
}

// ---------------------------------------------------------------------------
// OTel observable gauges — one shared, atomically-updated snapshot the
// meter's periodic collection callbacks read from, the same shape as the Go
// reference's mutex-guarded `latest sample` plus RegisterCallback closure.
// ---------------------------------------------------------------------------

struct GaugeState {
    std::atomic<double> cpu_pct{0};
    std::atomic<double> mem_pct{0};
    std::atomic<double> load1{0};
    std::string node;
};

namespace metrics_api = opentelemetry::metrics;

void observe_cpu(metrics_api::ObserverResult result, void* state) {
    auto* st = static_cast<GaugeState*>(state);
    if (auto* p = opentelemetry::nostd::get_if<
            opentelemetry::nostd::shared_ptr<metrics_api::ObserverResultT<double>>>(&result)) {
        (*p)->Observe(st->cpu_pct.load(std::memory_order_relaxed), {{"node", st->node}});
    }
}

void observe_mem(metrics_api::ObserverResult result, void* state) {
    auto* st = static_cast<GaugeState*>(state);
    if (auto* p = opentelemetry::nostd::get_if<
            opentelemetry::nostd::shared_ptr<metrics_api::ObserverResultT<double>>>(&result)) {
        (*p)->Observe(st->mem_pct.load(std::memory_order_relaxed), {{"node", st->node}});
    }
}

void observe_load(metrics_api::ObserverResult result, void* state) {
    auto* st = static_cast<GaugeState*>(state);
    if (auto* p = opentelemetry::nostd::get_if<
            opentelemetry::nostd::shared_ptr<metrics_api::ObserverResultT<double>>>(&result)) {
        (*p)->Observe(st->load1.load(std::memory_order_relaxed), {{"node", st->node}});
    }
}

} // namespace

int run(const std::string& node, int interval_ms, bool once, telemetry::Handle& tel) {
    GaugeState gauge_state;
    gauge_state.node = node;

    opentelemetry::nostd::shared_ptr<metrics_api::ObservableInstrument> cpu_gauge;
    opentelemetry::nostd::shared_ptr<metrics_api::ObservableInstrument> mem_gauge;
    opentelemetry::nostd::shared_ptr<metrics_api::ObservableInstrument> load_gauge;
    if (tel.enabled) {
        cpu_gauge = tel.meter->CreateDoubleObservableGauge(
            "sysagent.cpu.pct", "CPU utilization percent, from /proc/stat");
        mem_gauge = tel.meter->CreateDoubleObservableGauge(
            "sysagent.mem.pct", "Memory-used percent, from /proc/meminfo");
        load_gauge = tel.meter->CreateDoubleObservableGauge("sysagent.load1", "1-minute load average, from /proc/loadavg");
        cpu_gauge->AddCallback(observe_cpu, &gauge_state);
        mem_gauge->AddCallback(observe_mem, &gauge_state);
        load_gauge->AddCallback(observe_load, &gauge_state);
    }

    auto& sig = util::install_signal_flag();

    CpuTimes prev{};
    // Prime the CPU-time baseline so the first printed sample isn't a bogus 0
    // (matches Go: read once, sleep 200ms, THEN start the sample loop).
    if (auto t0 = read_cpu_times()) {
        prev = *t0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    int rc = 0;
    for (;;) {
        auto s = take_sample(prev);
        if (!s) {
            std::println(stderr, "sysagent: sample: {}", s.error());
            rc = 1;
            break;
        }
        if (tel.enabled) {
            gauge_state.cpu_pct.store(s->cpu_pct, std::memory_order_relaxed);
            gauge_state.mem_pct.store(s->mem_pct, std::memory_order_relaxed);
            gauge_state.load1.store(s->load1, std::memory_order_relaxed);
        }
        const auto ts = static_cast<long long>(std::time(nullptr));
        std::println("sysagent: node={} cpu_pct={:.2f} mem_pct={:.2f} load1={:.2f} ts={}", node,
                      s->cpu_pct, s->mem_pct, s->load1, ts);
        std::fflush(stdout);

        if (once) {
            break;
        }
        if (sig.load(std::memory_order_relaxed)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        if (sig.load(std::memory_order_relaxed)) {
            break;
        }
    }

    // Stop the callbacks from touching gauge_state before it goes out of
    // scope: a periodic collection could otherwise fire concurrently with
    // (or after) this function returning.
    if (tel.enabled) {
        cpu_gauge->RemoveCallback(observe_cpu, &gauge_state);
        mem_gauge->RemoveCallback(observe_mem, &gauge_state);
        load_gauge->RemoveCallback(observe_load, &gauge_state);
        std::println(stderr, "sysagent: otel export_errors={}", telemetry::export_errors.load());
    }
    return rc;
}

// saturate — chaos helper: busy-spin threads (cpu) or hold allocated, touched
// memory (mem) for --seconds, so a concurrently-running sysagent's
// cpu_pct/mem_pct visibly rises (this fleet's USE-method callback).
int saturate(const std::string& resource, int seconds, int workers, int mb) {
    if (resource == "cpu") {
        if (workers <= 0) {
            workers = static_cast<int>(std::thread::hardware_concurrency());
            if (workers <= 0) {
                workers = 1;
            }
        }
        std::println("sysagent: saturate resource=cpu seconds={} workers={} started", seconds, workers);
        std::fflush(stdout);
        const auto stop = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        std::vector<std::jthread> pool;
        pool.reserve(static_cast<std::size_t>(workers));
        for (int i = 0; i < workers; ++i) {
            pool.emplace_back([stop] {
                volatile double x = 0.0001;
                while (std::chrono::steady_clock::now() < stop) {
                    x = x * 1.0000001 + 1.0;
                }
            });
        }
        pool.clear(); // jthread destructors join
        std::println("sysagent: saturate done");
        return 0;
    }
    if (resource == "mem") {
        if (mb <= 0) {
            mb = 256;
        }
        std::println("sysagent: saturate resource=mem seconds={} mb={} started", seconds, mb);
        std::fflush(stdout);
        const std::size_t n = static_cast<std::size_t>(mb) * 1024 * 1024;
        std::vector<std::uint8_t> buf(n);
        for (std::size_t i = 0; i < n; ++i) {
            buf[i] = static_cast<std::uint8_t>(i); // touch every page so it's resident
        }
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        std::println("sysagent: saturate done");
        (void)buf[0];
        return 0;
    }
    std::println(stderr, "usage: sysagent saturate --resource cpu|mem --seconds N [--workers K|--mb M]");
    return 2;
}

} // namespace sysagent
