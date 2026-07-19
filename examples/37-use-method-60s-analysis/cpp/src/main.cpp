// loadmix (chapter 37): a saturation generator + guided-analysis harness for
// the classic Brendan Gregg 60-second USE-method checklist.
//
//   app --resource cpu|mem|io|net --seconds N   saturate that resource
//   app analyze [--seconds N]                    run the checklist and name
//                                                 the saturated resource
//
// `analyze` shells out to the real system tools (uptime, dmesg, vmstat,
// mpstat, pidstat, iostat, free, sar, ss, top) exactly as a human running the
// checklist would, parses their plain-text tables generically (by column
// *name*, read from each tool's own header row, never a hardcoded position),
// and for each of the four resources reports which USE signals — Utilization
// and Saturation — fired against a fixed threshold. Errors is reported too,
// fixed false: this chapter induces load, not kernel-logged faults, so an
// honest checklist run finds none.
//
// C++23 idioms used throughout: std::jthread (RAII-joined worker pools),
// std::atomic counters shared across them, std::expected for the one truly
// fallible operation (spawning a subprocess and reading its output), and
// std::println for all output.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <fstream>
#include <optional>
#include <print>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// ------------------------------------------------------------- utilities --

unsigned nproc() {
    unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 1 : n;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> split_ws(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream iss(line);
    std::string tok;
    while (iss >> tok) out.push_back(tok);
    return out;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) out.push_back(line);
    return out;
}

// A parsed table row: column names (from a tool's own header line) zipped
// with this row's values, so every parser below reads fields *by name*
// rather than by a hardcoded position — robust to sysstat/procps column
// reordering across versions.
struct Row {
    std::vector<std::string> names;
    std::vector<std::string> values;

    [[nodiscard]] std::optional<double> get(const std::string& name) const {
        for (size_t i = 0; i < names.size() && i < values.size(); ++i) {
            if (names[i] == name) {
                try {
                    return std::stod(values[i]);
                } catch (...) {
                    return std::nullopt;
                }
            }
        }
        return std::nullopt;
    }
};

Row make_row(const std::vector<std::string>& header_tokens, size_t header_skip,
             const std::vector<std::string>& data_tokens, size_t data_skip) {
    Row r;
    for (size_t i = header_skip; i < header_tokens.size(); ++i) r.names.push_back(header_tokens[i]);
    for (size_t i = data_skip; i < data_tokens.size(); ++i) r.values.push_back(data_tokens[i]);
    return r;
}

// Run a short command to completion and capture combined stdout+stderr.
// Forced to the C locale so decimal points/dates are unambiguous regardless
// of the guest's configured locale. The one fallible operation in this file
// that returns std::expected, per book convention.
std::expected<std::string, std::error_code> run_capture(const std::string& cmd) {
    // Grouped in a subshell so a single trailing "2>&1" captures every stage
    // of a pipeline (e.g. "dmesg | tail -n 5") — without the parens, 2>&1
    // binds only to the pipeline's last stage and an earlier stage's stderr
    // (e.g. dmesg's own permission-denied message) leaks past the capture.
    std::string full = "( env LC_ALL=C LANG=C " + cmd + " ) 2>&1";
    FILE* f = ::popen(full.c_str(), "r");
    if (!f) return std::unexpected(std::error_code{errno, std::system_category()});
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    ::pclose(f);
    return out;
}

// Start `env LC_ALL=C LANG=C <argv...>` in the background, stdout+stderr
// redirected to outfile; returns the child pid (or -1 on fork failure). Used
// for the five interval samplers so they run *concurrently* — a 12s analyze
// window costs ~12s wall-clock, not 5x that from running them one at a time.
pid_t spawn_to_file(const std::vector<std::string>& argv, const std::string& outfile) {
    pid_t pid = ::fork();
    if (pid == 0) {
        int fd = ::open(outfile.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd >= 0) {
            ::dup2(fd, STDOUT_FILENO);
            ::dup2(fd, STDERR_FILENO);
            ::close(fd);
        }
        std::vector<std::string> full = {"env", "LC_ALL=C", "LANG=C"};
        full.insert(full.end(), argv.begin(), argv.end());
        std::vector<char*> cargv;
        cargv.reserve(full.size() + 1);
        for (auto& s : full) cargv.push_back(s.data());
        cargv.push_back(nullptr);
        ::execvp("env", cargv.data());
        ::_exit(127);
    }
    return pid;
}

void wait_for(pid_t pid) {
    if (pid <= 0) return;
    int status = 0;
    ::waitpid(pid, &status, 0);
}

std::string read_all(const std::string& path) {
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ------------------------------------------------------- checklist parsers --
// Each parser reads a header line's column names from the tool's own output
// and zips subsequent data lines against that name list — see Row::get above.

struct VmstatResult {
    double run_queue_max = 0.0;
    double swap_io_max = 0.0; // si+so, KB/s
};

// vmstat has no aggregate "Average:" line (unlike mpstat/pidstat), and its
// *first* sample is a since-boot average, not a live interval — both quirks
// are handled here: we scan every data row after the header, skip row 0, and
// take the max across the rest (the steady-state saturation signal, not
// diluted by an average across the whole window).
VmstatResult parse_vmstat(const std::string& text) {
    VmstatResult res;
    std::vector<std::string> header;
    std::vector<std::vector<std::string>> data_rows;
    for (auto& line : split_lines(text)) {
        auto toks = split_ws(line);
        if (toks.empty()) continue;
        if (header.empty()) {
            bool has_swpd = std::any_of(toks.begin(), toks.end(), [](auto& t) { return t == "swpd"; });
            if (has_swpd) header = toks;
            continue;
        }
        if (toks.size() + 2 >= header.size()) data_rows.push_back(toks);
    }
    for (size_t i = 1; i < data_rows.size(); ++i) { // skip row 0 (since-boot average)
        Row r = make_row(header, 0, data_rows[i], 0);
        res.run_queue_max = std::max(res.run_queue_max, r.get("r").value_or(0.0));
        double si = r.get("si").value_or(0.0), so = r.get("so").value_or(0.0);
        res.swap_io_max = std::max(res.swap_io_max, si + so);
    }
    return res;
}

struct MpstatResult {
    double busy_pct = 0.0;
    double iowait_pct = 0.0;
};

// mpstat -P ALL's own "Average:" row over the whole window (excludes the
// since-boot first report automatically — sysstat's job, not ours).
MpstatResult parse_mpstat(const std::string& text) {
    MpstatResult res;
    std::vector<std::string> header, data;
    for (auto& line : split_lines(text)) {
        auto toks = split_ws(line);
        if (toks.size() < 2) continue;
        if (toks[0] == "Average:" && toks[1] == "CPU") header = toks;
        else if (toks[0] == "Average:" && toks[1] == "all") data = toks;
    }
    if (header.empty() || data.empty()) return res;
    Row r = make_row(header, 2, data, 2);
    double idle = r.get("%idle").value_or(100.0);
    res.iowait_pct = r.get("%iowait").value_or(0.0);
    res.busy_pct = 100.0 - idle;
    return res;
}

double parse_free_used_pct(const std::string& text) {
    std::vector<std::string> header, data;
    for (auto& line : split_lines(text)) {
        auto toks = split_ws(line);
        if (toks.empty()) continue;
        bool has_available = std::any_of(toks.begin(), toks.end(), [](auto& t) { return t == "available"; });
        if (has_available) header = toks;
        else if (toks[0] == "Mem:") data = toks;
    }
    if (header.empty() || data.empty()) return 0.0;
    Row r = make_row(header, 0, data, 1);
    double total = r.get("total").value_or(0.0);
    double avail = r.get("available").value_or(0.0);
    if (total <= 0.0) return 0.0;
    return (total - avail) / total * 100.0;
}

struct IostatResult {
    double util_max = 0.0;
    double await_max = 0.0; // w_await, ms
};

// iostat -xz has no Average line either, AND -z hides idle devices entirely
// (they just don't appear in a quiet interval) — so we scan every per-second
// block, skip block 0 (since-boot), skip loopback/removable/compressed
// pseudo-devices, and take the max over the guest's real block device.
IostatResult parse_iostat(const std::string& text) {
    IostatResult res;
    static const std::vector<std::string> excl = {"zram", "sr", "loop", "dm-"};
    int block_idx = -1;
    std::vector<std::string> header;
    for (auto& line : split_lines(text)) {
        auto toks = split_ws(line);
        if (toks.empty()) continue;
        if (toks[0] == "avg-cpu:") {
            ++block_idx;
            continue;
        }
        if (toks[0] == "Device") {
            header = toks;
            continue;
        }
        if (header.empty() || block_idx < 1) continue; // no header yet, or since-boot block
        const std::string& dev = toks[0];
        bool skip = std::any_of(excl.begin(), excl.end(), [&](auto& p) { return dev.rfind(p, 0) == 0; });
        if (skip) continue;
        Row r = make_row(header, 1, toks, 1);
        res.util_max = std::max(res.util_max, r.get("%util").value_or(0.0));
        res.await_max = std::max(res.await_max, r.get("w_await").value_or(0.0));
    }
    return res;
}

double parse_sar_pkts(const std::string& text, const std::string& iface) {
    std::vector<std::string> header, data;
    for (auto& line : split_lines(text)) {
        auto toks = split_ws(line);
        if (toks.size() < 2) continue;
        if (toks[0] == "Average:" && toks[1] == "IFACE") header = toks;
        else if (toks[0] == "Average:" && toks[1] == iface) data = toks;
    }
    if (header.empty() || data.empty()) return 0.0;
    Row r = make_row(header, 2, data, 2);
    return r.get("rxpck/s").value_or(0.0) + r.get("txpck/s").value_or(0.0);
}

// ---------------------------------------------------------------- saturate --

std::pair<uint64_t, uint64_t> sat_cpu(int seconds, unsigned workers) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::atomic<uint64_t> total_iters{0}, checksum{0};
    {
        std::vector<std::jthread> pool;
        pool.reserve(workers);
        for (unsigned w = 0; w < workers; ++w) {
            pool.emplace_back([&total_iters, &checksum, deadline, w](std::stop_token st) {
                uint64_t x = 0x9E3779B97F4A7C15ULL ^ (w + 1);
                uint64_t iters = 0;
                while (!st.stop_requested()) {
                    x ^= x << 7;
                    x ^= x >> 9;
                    ++iters;
                    if ((iters & 0xFFFFF) == 0 && std::chrono::steady_clock::now() >= deadline) break;
                }
                total_iters.fetch_add(iters, std::memory_order_relaxed);
                checksum.fetch_add(x, std::memory_order_relaxed); // keeps x provably live (no DCE)
            });
        }
    } // jthreads join here (RAII)
    return {total_iters.load(), checksum.load()};
}

std::pair<uint64_t, uint64_t> sat_mem(int seconds, unsigned workers) {
    uint64_t mem_kb = 0;
    {
        std::ifstream mi("/proc/meminfo");
        std::string line;
        while (std::getline(mi, line)) {
            if (line.rfind("MemTotal:", 0) == 0) {
                std::istringstream iss(line.substr(9));
                iss >> mem_kb;
                break;
            }
        }
    }
    uint64_t target_bytes = mem_kb > 0 ? static_cast<uint64_t>(mem_kb * 1024.0 * 1.35)
                                       : (2ull << 30); // 2 GiB fallback if /proc/meminfo is unreadable
    uint64_t chunk = std::max<uint64_t>(target_bytes / workers, 4096 * 16);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::atomic<uint64_t> total_touches{0};
    {
        std::vector<std::jthread> pool;
        pool.reserve(workers);
        for (unsigned w = 0; w < workers; ++w) {
            pool.emplace_back([&total_touches, deadline, chunk](std::stop_token st) {
                std::vector<uint8_t> buf(chunk, 0);
                uint64_t touches = 0;
                while (!st.stop_requested() && std::chrono::steady_clock::now() < deadline) {
                    for (size_t off = 0; off < buf.size(); off += 4096) {
                        buf[off] = static_cast<uint8_t>(buf[off] + 1);
                        ++touches;
                    }
                }
                total_touches.fetch_add(touches, std::memory_order_relaxed);
            });
        }
    }
    return {total_touches.load(), target_bytes};
}

std::pair<uint64_t, uint64_t> sat_io(int seconds, unsigned workers) {
    std::string dir = "/var/tmp/loadmix-io-" + std::to_string(::getpid());
    ::mkdir(dir.c_str(), 0700);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::atomic<uint64_t> total_writes{0}, total_bytes{0};
    {
        std::vector<std::jthread> pool;
        pool.reserve(workers);
        for (unsigned w = 0; w < workers; ++w) {
            std::string path = dir + "/w" + std::to_string(w) + ".dat";
            pool.emplace_back([&total_writes, &total_bytes, deadline, path](std::stop_token st) {
                int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_DSYNC, 0600);
                if (fd < 0) return;
                constexpr size_t kBuf = 16384;
                std::vector<uint8_t> buf(kBuf);
                // xorshift64*: cheap per-thread PRNG so the payload isn't
                // trivially compressible (this guest's /var is btrfs with
                // zstd:1 compression, which would otherwise let a real disk
                // write shrink to nearly nothing and mute the iowait signal).
                uint64_t seed = 0x2545F4914F6CDD1DULL ^ reinterpret_cast<uintptr_t>(buf.data());
                uint64_t writes = 0, bytes = 0;
                while (!st.stop_requested() && std::chrono::steady_clock::now() < deadline) {
                    for (size_t i = 0; i < kBuf; i += 8) {
                        seed ^= seed << 13;
                        seed ^= seed >> 7;
                        seed ^= seed << 17;
                        std::memcpy(buf.data() + i, &seed, 8);
                    }
                    ssize_t n = ::pwrite(fd, buf.data(), buf.size(), 0); // same 16 KiB region: bounded file size
                    if (n > 0) {
                        ++writes;
                        bytes += static_cast<uint64_t>(n);
                    }
                }
                ::close(fd);
                ::unlink(path.c_str());
                total_writes.fetch_add(writes, std::memory_order_relaxed);
                total_bytes.fetch_add(bytes, std::memory_order_relaxed);
            });
        }
    }
    ::rmdir(dir.c_str());
    return {total_writes.load(), total_bytes.load()};
}

std::pair<uint64_t, uint64_t> sat_net(int seconds, unsigned workers) {
    int rfd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(rfd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    socklen_t alen = sizeof addr;
    ::getsockname(rfd, reinterpret_cast<sockaddr*>(&addr), &alen);
    uint16_t port = ntohs(addr.sin_port);
    timeval tv{0, 200000}; // 200ms so the receiver notices `stop` promptly
    ::setsockopt(rfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> total_pkts{0}, total_bytes{0};
    {
        std::jthread receiver([&stop, rfd](std::stop_token) {
            char buf[256];
            while (!stop.load(std::memory_order_relaxed)) ::recv(rfd, buf, sizeof buf, 0);
        });
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        {
            std::vector<std::jthread> pool;
            pool.reserve(workers);
            for (unsigned w = 0; w < workers; ++w) {
                pool.emplace_back([&total_pkts, &total_bytes, deadline, port](std::stop_token st) {
                    int sfd = ::socket(AF_INET, SOCK_DGRAM, 0);
                    sockaddr_in dst{};
                    dst.sin_family = AF_INET;
                    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                    dst.sin_port = htons(port);
                    ::connect(sfd, reinterpret_cast<sockaddr*>(&dst), sizeof dst);
                    char payload[64] = {};
                    uint64_t pkts = 0, bytes = 0;
                    while (!st.stop_requested() && std::chrono::steady_clock::now() < deadline) {
                        ssize_t n = ::send(sfd, payload, sizeof payload, 0);
                        if (n > 0) {
                            ++pkts;
                            bytes += static_cast<uint64_t>(n);
                        }
                    }
                    ::close(sfd);
                    total_pkts.fetch_add(pkts, std::memory_order_relaxed);
                    total_bytes.fetch_add(bytes, std::memory_order_relaxed);
                });
            }
        } // sender pool joins here
        stop.store(true, std::memory_order_relaxed);
    } // receiver joins here (notices `stop` within ~200ms)
    ::close(rfd);
    return {total_pkts.load(), total_bytes.load()};
}

int cmd_saturate(const std::string& resource, int seconds) {
    unsigned cpus = nproc();
    unsigned workers = (resource == "mem") ? std::max(2u, cpus) : std::max(4u, cpus * (resource == "cpu" ? 3u : 4u));

    std::println("loadmix: start resource={} seconds={} workers={}", resource, seconds, workers);
    std::pair<uint64_t, uint64_t> r;
    if (resource == "cpu") r = sat_cpu(seconds, workers);
    else if (resource == "mem") r = sat_mem(seconds, workers);
    else if (resource == "io") r = sat_io(seconds, workers);
    else r = sat_net(seconds, workers);
    std::println("loadmix: done resource={} seconds={} workers={} ops={} bytes={}", resource, seconds, workers,
                  r.first, r.second);
    return 0;
}

// ------------------------------------------------------------------ analyze --

int cmd_analyze(int seconds) {
    unsigned cpus = nproc();
    std::println("analyze: start seconds={} cpus={}", seconds, cpus);

    auto up = run_capture("uptime");
    std::println("analyze: tool uptime raw=\"{}\"", up ? trim(*up) : std::string("unavailable"));

    auto dm = run_capture("dmesg | tail -n 5");
    std::string dm_status = "ok";
    if (!dm || dm->find("Operation not permitted") != std::string::npos) dm_status = "denied";
    std::println("analyze: tool dmesg status={}", dm_status);

    auto top_out = run_capture("top -b -n1");
    std::string cpu_line, tasks_line;
    if (top_out) {
        for (auto& l : split_lines(*top_out)) {
            if (l.rfind("%Cpu", 0) == 0) cpu_line = trim(l);
            if (l.rfind("Tasks:", 0) == 0) tasks_line = trim(l);
        }
    }
    std::println("analyze: tool top cpu=\"{}\" tasks=\"{}\"", cpu_line, tasks_line);

    auto ss_out = run_capture("ss -s");
    std::string ss_line;
    if (ss_out) {
        auto lines = split_lines(*ss_out);
        if (!lines.empty()) ss_line = trim(lines[0]);
    }
    std::println("analyze: tool ss raw=\"{}\"", ss_line);

    // The five interval samplers run CONCURRENTLY (each redirected to its own
    // temp file) so an N-second checklist costs ~N seconds, not 5N.
    std::string dir = "/tmp/loadmix-analyze-" + std::to_string(::getpid());
    ::mkdir(dir.c_str(), 0700);
    std::string vfile = dir + "/vmstat.out", mfile = dir + "/mpstat.out", ifile = dir + "/iostat.out",
                sfile = dir + "/sar.out", pfile = dir + "/pidstat.out";
    std::string n = std::to_string(seconds);
    pid_t pv = spawn_to_file({"vmstat", "1", n}, vfile);
    pid_t pm = spawn_to_file({"mpstat", "-P", "ALL", "1", n}, mfile);
    pid_t pi = spawn_to_file({"iostat", "-xz", "1", n}, ifile);
    pid_t ps = spawn_to_file({"sar", "-n", "DEV", "1", n}, sfile);
    pid_t pp = spawn_to_file({"pidstat", "1", n}, pfile);
    wait_for(pv);
    wait_for(pm);
    wait_for(pi);
    wait_for(ps);
    wait_for(pp);
    std::string vtext = read_all(vfile), mtext = read_all(mfile), itext = read_all(ifile), stext = read_all(sfile),
                ptext = read_all(pfile);
    ::unlink(vfile.c_str());
    ::unlink(mfile.c_str());
    ::unlink(ifile.c_str());
    ::unlink(sfile.c_str());
    ::unlink(pfile.c_str());
    ::rmdir(dir.c_str());

    int active = 0;
    for (auto& l : split_lines(ptext)) {
        auto t = split_ws(l);
        if (t.size() >= 2 && t[0] == "Average:" && t[1] != "UID") ++active;
    }
    std::println("analyze: tool pidstat active_processes={}", active);

    auto free_out = run_capture("free -m");
    double mem_used_pct = free_out ? parse_free_used_pct(*free_out) : 0.0;

    VmstatResult vm = parse_vmstat(vtext);
    MpstatResult mp = parse_mpstat(mtext);
    IostatResult io = parse_iostat(itext);
    double net_pkts = parse_sar_pkts(stext, "lo");

    std::println("analyze: metric resource=cpu name=busy_pct value={:.2f} unit=pct", mp.busy_pct);
    std::println("analyze: metric resource=cpu name=run_queue value={:.2f} unit=procs", vm.run_queue_max);
    std::println("analyze: metric resource=mem name=used_pct value={:.2f} unit=pct", mem_used_pct);
    std::println("analyze: metric resource=mem name=swap_io value={:.2f} unit=kbps", vm.swap_io_max);
    std::println("analyze: metric resource=io name=util_pct value={:.2f} unit=pct", io.util_max);
    std::println("analyze: metric resource=io name=iowait_pct value={:.2f} unit=pct", mp.iowait_pct);
    std::println("analyze: metric resource=io name=await_ms value={:.2f} unit=ms", io.await_max);
    std::println("analyze: metric resource=net name=pkts value={:.2f} unit=per_s", net_pkts);

    // signal thresholds: Utilization/Saturation pair per resource. cpu's
    // saturation threshold scales with this guest's own core count — a run
    // queue longer than (cpus + headroom) means threads are waiting for a
    // CPU, the textbook USE definition of saturation.
    struct Candidate {
        std::string resource;
        double u_val, u_thr, s_val, s_thr;
    };
    std::vector<Candidate> table = {
        {"cpu", mp.busy_pct, 60.0, vm.run_queue_max, cpus + 2.0},
        {"mem", mem_used_pct, 75.0, vm.swap_io_max, 300.0},
        {"io", io.util_max, 40.0, mp.iowait_pct, 8.0},
        {"net", net_pkts, 2000.0, net_pkts, 8000.0},
    };

    std::string verdict;
    double best_ratio = -1.0;
    for (auto& c : table) {
        bool ufired = c.u_val >= c.u_thr;
        bool sfired = c.s_val >= c.s_thr;
        std::println("analyze: signal resource={} type=Utilization fired={} value={:.2f} threshold={:.2f}",
                      c.resource, ufired ? "true" : "false", c.u_val, c.u_thr);
        std::println("analyze: signal resource={} type=Saturation fired={} value={:.2f} threshold={:.2f}",
                      c.resource, sfired ? "true" : "false", c.s_val, c.s_thr);
        std::println("analyze: signal resource={} type=Errors fired=false", c.resource);
        double ratio = c.s_val / c.s_thr;
        if (ratio > best_ratio) {
            best_ratio = ratio;
            verdict = c.resource;
        }
    }
    std::println("analyze: verdict resource={} ratio={:.2f}", verdict, best_ratio);
    std::println("analyze: done");
    return 0;
}

void usage() {
    std::println(stderr, "usage: app --resource cpu|mem|io|net --seconds N");
    std::println(stderr, "       app analyze [--seconds N]");
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);

    if (!args.empty() && args[0] == "analyze") {
        int seconds = 60;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--seconds" && i + 1 < args.size()) {
                try {
                    seconds = std::stoi(args[++i]);
                } catch (...) {
                    usage();
                    return 2;
                }
            } else {
                usage();
                return 2;
            }
        }
        if (seconds <= 0) {
            usage();
            return 2;
        }
        return cmd_analyze(seconds);
    }

    std::string resource;
    int seconds = -1;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--resource" && i + 1 < args.size()) {
            resource = args[++i];
        } else if (args[i] == "--seconds" && i + 1 < args.size()) {
            try {
                seconds = std::stoi(args[++i]);
            } catch (...) {
                usage();
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }
    if (resource != "cpu" && resource != "mem" && resource != "io" && resource != "net") {
        usage();
        return 2;
    }
    if (seconds <= 0) {
        usage();
        return 2;
    }
    return cmd_saturate(resource, seconds);
}
