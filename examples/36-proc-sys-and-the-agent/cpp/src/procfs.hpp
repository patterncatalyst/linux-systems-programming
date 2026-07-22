// sysagent: types + readers for /proc, /sys/fs/cgroup PSI, and the snapshot
// they compose into. See procfs.cpp for the parsing.
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace sysagent {

struct DiskStat {
    std::string name;
    int64_t reads = 0;
    int64_t writes = 0;
    int64_t read_sectors = 0;
    int64_t write_sectors = 0;
};

struct NetStat {
    std::string iface;
    int64_t rx_bytes = 0;
    int64_t tx_bytes = 0;
};

// One fully-formed sample. Field names here are the canonical, deterministic
// names shared with the Go and Rust implementations (see README.md).
struct Snapshot {
    double cpu_util_pct = 0.0;
    double cpu_user_pct = 0.0;
    double cpu_system_pct = 0.0;

    double load1 = 0.0;
    double load5 = 0.0;
    double load15 = 0.0;
    int64_t runnable = 0;
    int64_t total_threads = 0;

    int64_t mem_total_kb = 0;
    int64_t mem_available_kb = 0;
    int64_t mem_used_kb = 0;

    std::vector<DiskStat> disks;
    std::vector<NetStat> net;

    bool psi_available = false;
    double psi_cpu_some_avg10 = 0.0;
    double psi_mem_some_avg10 = 0.0;
    double psi_io_some_avg10 = 0.0;
};

// Raw /proc/stat "cpu " aggregate line, in jiffies.
struct CpuTicks {
    int64_t user = 0;
    int64_t nice = 0;
    int64_t system = 0;
    int64_t idle = 0;
    int64_t iowait = 0;
    int64_t irq = 0;
    int64_t softirq = 0;
    int64_t steal = 0;
};

[[nodiscard]] std::expected<CpuTicks, std::string> read_cpu_ticks();

struct LoadAvg {
    double load1 = 0.0;
    double load5 = 0.0;
    double load15 = 0.0;
    int64_t runnable = 0;
    int64_t total_threads = 0;
};
[[nodiscard]] std::expected<LoadAvg, std::string> read_loadavg();

struct MemInfo {
    int64_t total_kb = 0;
    int64_t available_kb = 0;
};
[[nodiscard]] std::expected<MemInfo, std::string> read_meminfo();

[[nodiscard]] std::expected<std::vector<DiskStat>, std::string> read_diskstats();
[[nodiscard]] std::expected<std::vector<NetStat>, std::string> read_netdev();

struct Psi {
    double cpu_some_avg10 = 0.0;
    double mem_some_avg10 = 0.0;
    double io_some_avg10 = 0.0;
};
// Cgroup-scoped PSI when the calling process's cgroup exposes pressure files,
// falling back to system-wide /proc/pressure/*. std::nullopt if PSI is not
// available on this kernel/host at all (no error text: this is expected).
[[nodiscard]] std::optional<Psi> read_psi();

// Take deltas of two CpuTicks readings `interval_ms` apart and fill in every
// field of a Snapshot. This is the one function main.cpp calls for `sample`.
[[nodiscard]] std::expected<Snapshot, std::string> take_snapshot(int interval_ms);

// Render as the deterministic single-line JSON object (see README.md).
[[nodiscard]] std::string to_json(const Snapshot& s);
// Render as human-readable key=value lines.
[[nodiscard]] std::string to_text(const Snapshot& s);

} // namespace sysagent
