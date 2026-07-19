#include "procfs.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

using namespace std::chrono_literals;

namespace sysagent {

namespace {

// RAII wrapper around an ifstream: names the path (for error messages) and
// guarantees the fd is closed when the wrapper goes out of scope, exactly
// like every other UniqueFd-style RAII type in this book — just for the
// stdio-buffered stream form /proc parsing normally uses.
class ProcFile {
  public:
    explicit ProcFile(std::string path) : path_(std::move(path)), stream_(path_) {}

    [[nodiscard]] bool ok() const { return stream_.is_open(); }
    [[nodiscard]] const std::string& path() const { return path_; }
    std::ifstream& stream() { return stream_; }

  private:
    std::string path_;
    std::ifstream stream_;
};

[[nodiscard]] std::string open_error(const ProcFile& f) {
    return "cannot open " + f.path();
}

// double from a "some avg10=X.XX avg60=Y.YY avg300=Z.ZZ total=N" PSI line.
double parse_avg10(const std::string& line) {
    double v = 0.0;
    std::sscanf(line.c_str(), "%*s avg10=%lf", &v);
    return v;
}

std::optional<Psi> read_psi_from_dir(const std::string& dir) {
    Psi psi{};
    bool any = false;
    for (auto [file, out] : {
             std::pair{"cpu.pressure", &psi.cpu_some_avg10},
             std::pair{"memory.pressure", &psi.mem_some_avg10},
             std::pair{"io.pressure", &psi.io_some_avg10},
         }) {
        ProcFile f(dir + "/" + file);
        if (!f.ok()) continue;
        std::string line;
        if (std::getline(f.stream(), line)) {
            *out = parse_avg10(line);
            any = true;
        }
    }
    return any ? std::optional{psi} : std::nullopt;
}

std::optional<Psi> read_psi_from_proc_pressure() {
    Psi psi{};
    bool any = false;
    for (auto [file, out] : {
             std::pair{"/proc/pressure/cpu", &psi.cpu_some_avg10},
             std::pair{"/proc/pressure/memory", &psi.mem_some_avg10},
             std::pair{"/proc/pressure/io", &psi.io_some_avg10},
         }) {
        ProcFile f(file);
        if (!f.ok()) continue;
        std::string line;
        if (std::getline(f.stream(), line)) {
            *out = parse_avg10(line);
            any = true;
        }
    }
    return any ? std::optional{psi} : std::nullopt;
}

} // namespace

std::expected<CpuTicks, std::string> read_cpu_ticks() {
    ProcFile f("/proc/stat");
    if (!f.ok()) return std::unexpected(open_error(f));
    std::string line;
    if (!std::getline(f.stream(), line)) {
        return std::unexpected("/proc/stat: empty");
    }
    // "cpu  user nice system idle iowait irq softirq steal guest guest_nice"
    std::istringstream iss(line);
    std::string label;
    CpuTicks t{};
    iss >> label >> t.user >> t.nice >> t.system >> t.idle >> t.iowait >> t.irq >> t.softirq >>
        t.steal;
    if (label != "cpu") return std::unexpected("/proc/stat: unexpected first line");
    return t;
}

std::expected<LoadAvg, std::string> read_loadavg() {
    ProcFile f("/proc/loadavg");
    if (!f.ok()) return std::unexpected(open_error(f));
    LoadAvg la{};
    std::string running_total;
    long long last_pid = 0;
    f.stream() >> la.load1 >> la.load5 >> la.load15 >> running_total >> last_pid;
    if (!f.stream() && !f.stream().eof()) return std::unexpected("/proc/loadavg: parse error");
    auto slash = running_total.find('/');
    if (slash == std::string::npos) return std::unexpected("/proc/loadavg: bad running/total field");
    la.runnable = std::stoll(running_total.substr(0, slash));
    la.total_threads = std::stoll(running_total.substr(slash + 1));
    return la;
}

std::expected<MemInfo, std::string> read_meminfo() {
    ProcFile f("/proc/meminfo");
    if (!f.ok()) return std::unexpected(open_error(f));
    MemInfo mi{};
    bool have_total = false, have_avail = false;
    std::string line;
    while (std::getline(f.stream(), line)) {
        std::istringstream iss(line);
        std::string key;
        int64_t kb = 0;
        iss >> key >> kb;
        if (key == "MemTotal:") {
            mi.total_kb = kb;
            have_total = true;
        } else if (key == "MemAvailable:") {
            mi.available_kb = kb;
            have_avail = true;
        }
        if (have_total && have_avail) break;
    }
    if (!have_total || !have_avail) return std::unexpected("/proc/meminfo: missing keys");
    return mi;
}

std::expected<std::vector<DiskStat>, std::string> read_diskstats() {
    ProcFile f("/proc/diskstats");
    if (!f.ok()) return std::unexpected(open_error(f));
    std::vector<DiskStat> out;
    std::string line;
    while (std::getline(f.stream(), line)) {
        std::istringstream iss(line);
        int64_t major = 0, minor = 0;
        std::string name;
        int64_t reads_completed = 0, reads_merged = 0, sectors_read = 0, ms_reading = 0;
        int64_t writes_completed = 0, writes_merged = 0, sectors_written = 0;
        iss >> major >> minor >> name >> reads_completed >> reads_merged >> sectors_read >>
            ms_reading >> writes_completed >> writes_merged >> sectors_written;
        if (name.empty()) continue;
        // Skip loop/ram devices and partitions with no traffic at all — keeps
        // the array focused on the disks that actually matter for USE-method
        // reads, without hardcoding any device-naming scheme.
        if (name.starts_with("loop") || name.starts_with("ram")) continue;
        out.push_back(DiskStat{
            .name = name,
            .reads = reads_completed,
            .writes = writes_completed,
            .read_sectors = sectors_read,
            .write_sectors = sectors_written,
        });
    }
    return out;
}

std::expected<std::vector<NetStat>, std::string> read_netdev() {
    ProcFile f("/proc/net/dev");
    if (!f.ok()) return std::unexpected(open_error(f));
    std::vector<NetStat> out;
    std::string line;
    int lineno = 0;
    while (std::getline(f.stream(), line)) {
        ++lineno;
        if (lineno <= 2) continue; // two header lines
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string iface = line.substr(0, colon);
        iface.erase(std::remove(iface.begin(), iface.end(), ' '), iface.end());
        std::istringstream iss(line.substr(colon + 1));
        int64_t rx_bytes = 0, tx_bytes = 0, field = 0;
        iss >> rx_bytes;
        for (int i = 0; i < 7; ++i) iss >> field; // rx packets..rx multicast
        iss >> tx_bytes;
        out.push_back(NetStat{.iface = iface, .rx_bytes = rx_bytes, .tx_bytes = tx_bytes});
    }
    return out;
}

std::optional<Psi> read_psi() {
    // Prefer this process's own cgroup (v2 unified hierarchy: a single
    // "0::/path" line in /proc/self/cgroup) so the numbers reflect exactly
    // the pressure this program's cgroup is under.
    ProcFile self_cg("/proc/self/cgroup");
    if (self_cg.ok()) {
        std::string line;
        while (std::getline(self_cg.stream(), line)) {
            if (!line.starts_with("0::")) continue;
            std::string rel = line.substr(3);
            if (auto psi = read_psi_from_dir("/sys/fs/cgroup" + rel)) return psi;
            break;
        }
    }
    // Fall back to the system-wide files (still cgroup2 PSI under the hood).
    return read_psi_from_proc_pressure();
}

std::expected<Snapshot, std::string> take_snapshot(int interval_ms) {
    auto t0 = read_cpu_ticks();
    if (!t0) return std::unexpected(t0.error());

    // Gather the non-CPU sources concurrently with the sampling sleep, using
    // jthreads (auto-joining, cooperatively cancellable) rather than raw
    // pthreads; ok_count is an atomic so every worker can report success
    // without a mutex.
    Snapshot snap{};
    std::atomic<int> ok_count{0};
    std::string first_error;
    std::mutex err_mu;
    auto note_error = [&](std::string msg) {
        std::lock_guard lock(err_mu);
        if (first_error.empty()) first_error = std::move(msg);
    };

    std::jthread load_thread([&] {
        if (auto la = read_loadavg()) {
            snap.load1 = la->load1;
            snap.load5 = la->load5;
            snap.load15 = la->load15;
            snap.runnable = la->runnable;
            snap.total_threads = la->total_threads;
            ok_count.fetch_add(1, std::memory_order_relaxed);
        } else {
            note_error(la.error());
        }
    });
    std::jthread mem_thread([&] {
        if (auto mi = read_meminfo()) {
            snap.mem_total_kb = mi->total_kb;
            snap.mem_available_kb = mi->available_kb;
            snap.mem_used_kb = mi->total_kb - mi->available_kb;
            ok_count.fetch_add(1, std::memory_order_relaxed);
        } else {
            note_error(mi.error());
        }
    });
    std::jthread disk_thread([&] {
        if (auto ds = read_diskstats()) {
            snap.disks = std::move(*ds);
            ok_count.fetch_add(1, std::memory_order_relaxed);
        } else {
            note_error(ds.error());
        }
    });
    std::jthread net_thread([&] {
        if (auto ns = read_netdev()) {
            snap.net = std::move(*ns);
            ok_count.fetch_add(1, std::memory_order_relaxed);
        } else {
            note_error(ns.error());
        }
    });
    std::jthread psi_thread([&] {
        if (auto psi = read_psi()) {
            snap.psi_available = true;
            snap.psi_cpu_some_avg10 = psi->cpu_some_avg10;
            snap.psi_mem_some_avg10 = psi->mem_some_avg10;
            snap.psi_io_some_avg10 = psi->io_some_avg10;
        }
        ok_count.fetch_add(1, std::memory_order_relaxed);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));

    // jthread destructors join as each goes out of scope at the end of this
    // block; explicitly joining here just makes the ordering-before-t1 clear.
    load_thread.join();
    mem_thread.join();
    disk_thread.join();
    net_thread.join();
    psi_thread.join();

    if (ok_count.load(std::memory_order_relaxed) < 4) {
        return std::unexpected(first_error.empty() ? "a /proc source failed" : first_error);
    }

    auto t1 = read_cpu_ticks();
    if (!t1) return std::unexpected(t1.error());

    auto busy0 = t0->user + t0->nice + t0->system;
    auto busy1 = t1->user + t1->nice + t1->system;
    auto idle0 = t0->idle + t0->iowait;
    auto idle1 = t1->idle + t1->iowait;
    auto total0 = busy0 + idle0 + t0->irq + t0->softirq + t0->steal;
    auto total1 = busy1 + idle1 + t1->irq + t1->softirq + t1->steal;

    auto d_total = static_cast<double>(total1 - total0);
    auto d_busy = static_cast<double>((busy1 - busy0) + (t1->irq - t0->irq) +
                                       (t1->softirq - t0->softirq) + (t1->steal - t0->steal));
    auto d_user = static_cast<double>(t1->user - t0->user);
    auto d_system = static_cast<double>(t1->system - t0->system);

    if (d_total > 0.0) {
        snap.cpu_util_pct = 100.0 * d_busy / d_total;
        snap.cpu_user_pct = 100.0 * d_user / d_total;
        snap.cpu_system_pct = 100.0 * d_system / d_total;
    }

    return snap;
}

namespace {

std::string fmt2(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

} // namespace

std::string to_json(const Snapshot& s) {
    std::ostringstream o;
    o << "{";
    o << "\"cpu_util_pct\":" << fmt2(s.cpu_util_pct) << ",";
    o << "\"cpu_user_pct\":" << fmt2(s.cpu_user_pct) << ",";
    o << "\"cpu_system_pct\":" << fmt2(s.cpu_system_pct) << ",";
    o << "\"load1\":" << fmt2(s.load1) << ",";
    o << "\"load5\":" << fmt2(s.load5) << ",";
    o << "\"load15\":" << fmt2(s.load15) << ",";
    o << "\"runnable\":" << s.runnable << ",";
    o << "\"total_threads\":" << s.total_threads << ",";
    o << "\"mem_total_kb\":" << s.mem_total_kb << ",";
    o << "\"mem_available_kb\":" << s.mem_available_kb << ",";
    o << "\"mem_used_kb\":" << s.mem_used_kb << ",";
    o << "\"disks\":[";
    for (size_t i = 0; i < s.disks.size(); ++i) {
        const auto& d = s.disks[i];
        if (i) o << ",";
        o << "{\"name\":\"" << d.name << "\",\"reads\":" << d.reads << ",\"writes\":" << d.writes
          << ",\"read_sectors\":" << d.read_sectors << ",\"write_sectors\":" << d.write_sectors
          << "}";
    }
    o << "],";
    o << "\"net\":[";
    for (size_t i = 0; i < s.net.size(); ++i) {
        const auto& n = s.net[i];
        if (i) o << ",";
        o << "{\"iface\":\"" << n.iface << "\",\"rx_bytes\":" << n.rx_bytes
          << ",\"tx_bytes\":" << n.tx_bytes << "}";
    }
    o << "],";
    o << "\"psi_available\":" << (s.psi_available ? "true" : "false") << ",";
    o << "\"psi_cpu_some_avg10\":" << fmt2(s.psi_cpu_some_avg10) << ",";
    o << "\"psi_mem_some_avg10\":" << fmt2(s.psi_mem_some_avg10) << ",";
    o << "\"psi_io_some_avg10\":" << fmt2(s.psi_io_some_avg10);
    o << "}";
    return o.str();
}

std::string to_text(const Snapshot& s) {
    std::ostringstream o;
    o << "cpu_util_pct=" << fmt2(s.cpu_util_pct) << " cpu_user_pct=" << fmt2(s.cpu_user_pct)
      << " cpu_system_pct=" << fmt2(s.cpu_system_pct) << "\n";
    o << "load1=" << fmt2(s.load1) << " load5=" << fmt2(s.load5) << " load15=" << fmt2(s.load15)
      << " runnable=" << s.runnable << " total_threads=" << s.total_threads << "\n";
    o << "mem_total_kb=" << s.mem_total_kb << " mem_available_kb=" << s.mem_available_kb
      << " mem_used_kb=" << s.mem_used_kb << "\n";
    for (const auto& d : s.disks) {
        o << "disk name=" << d.name << " reads=" << d.reads << " writes=" << d.writes
          << " read_sectors=" << d.read_sectors << " write_sectors=" << d.write_sectors << "\n";
    }
    for (const auto& n : s.net) {
        o << "net iface=" << n.iface << " rx_bytes=" << n.rx_bytes << " tx_bytes=" << n.tx_bytes
          << "\n";
    }
    o << "psi_available=" << (s.psi_available ? "true" : "false")
      << " psi_cpu_some_avg10=" << fmt2(s.psi_cpu_some_avg10)
      << " psi_mem_some_avg10=" << fmt2(s.psi_mem_some_avg10)
      << " psi_io_some_avg10=" << fmt2(s.psi_io_some_avg10) << "\n";
    return o.str();
}

} // namespace sysagent
