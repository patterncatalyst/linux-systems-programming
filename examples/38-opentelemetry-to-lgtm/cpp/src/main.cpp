// lsp-otel-cpp — a traced request/response loop (chapter 38: OpenTelemetry
// to LGTM).
//
//	app serve --port PORT [--otel-endpoint URL]
//	app drive --n N --addr HOST:PORT
//
// serve accepts one persistent line-oriented connection and treats every
// line as a "request": a parent span ("request") wraps two children
// ("work", "respond"), a monotonic counter (requests_total) is incremented,
// and a histogram (request_duration, milliseconds) records the parent
// span's wall time. drive is an untraced TCP client that sends N requests
// down one connection and waits for N replies -- the load generator for
// serve's telemetry, not a telemetry source itself.
//
// Every signal appears exactly once in the OTLP stream: serve's own
// SIGINT/SIGTERM handling flushes and shuts down the SDK before exit, the
// C++ way of RAII (an explicit flush_and_shutdown() call from the signal
// path, since a detached accept thread and a process-ending exit give no
// destructor a guaranteed chance to run -- the same reasoning the Rust port
// documents for its own explicit shutdown() call).
#include "telemetry.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <opentelemetry/context/context.h>
#include <opentelemetry/trace/scope.h>
#include <opentelemetry/trace/span.h>

namespace {

namespace trace_api = opentelemetry::trace;

[[noreturn]] void usage() {
    std::println(stderr, "usage: app serve --port PORT [--otel-endpoint URL]");
    std::println(stderr, "       app drive --n N --addr HOST:PORT");
    std::exit(2);
}

[[noreturn]] void die(const std::string& msg) {
    std::println(stderr, "app: error: {}", msg);
    std::exit(1);
}

std::string_view trim(std::string_view s) {
    const auto is_space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

// write_all loops over write(2) so short writes (always possible on a
// socket) never truncate a reply -- the syscall-level equivalent of Go's
// bufio.Writer.Flush() / Rust's Write::write_all().
bool write_all(int fd, std::string_view data) {
    std::size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

// LineReader is bufio.Scanner/BufReader::read_line's counterpart: it buffers
// raw read(2) chunks and hands back one newline-delimited line at a time,
// including a final unterminated line at EOF.
class LineReader {
public:
    explicit LineReader(int fd) : fd_(fd) {}

    bool read_line(std::string& out) {
        for (;;) {
            if (const auto pos = buf_.find('\n'); pos != std::string::npos) {
                out = buf_.substr(0, pos);
                buf_.erase(0, pos + 1);
                return true;
            }
            char chunk[4096];
            const ssize_t n = ::read(fd_, chunk, sizeof(chunk));
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (n == 0) {
                if (!buf_.empty()) {
                    out = std::move(buf_);
                    buf_.clear();
                    return true;
                }
                return false;
            }
            buf_.append(chunk, static_cast<std::size_t>(n));
        }
    }

private:
    int fd_;
    std::string buf_;
};

// parse_i64 mirrors Go's strconv.Atoi / Rust's str::parse::<i64>(): the
// WHOLE trimmed string must be a valid integer, not merely a prefix of one.
bool parse_i64(std::string_view s, std::int64_t& out) {
    if (s.empty()) {
        return false;
    }
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && ptr == s.data() + s.size();
}

bool parse_port(std::string_view s, std::uint16_t& out) {
    unsigned int v = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || ptr != s.data() + s.size() || v > 65535) {
        return false;
    }
    out = static_cast<std::uint16_t>(v);
    return true;
}

// ------------------------------------------------------------------- serve

// handle_request is the parse->work->respond pipeline for one line of
// input. "parse" happens inline in serve_conn (cheap: one from_chars call);
// "work" and "respond" are the two child spans under the "request" parent,
// per the trace contract every language in this example follows
// identically.
bool handle_request(const telemetry::Handle& t, int fd, std::int64_t seq) {
    const auto start = std::chrono::steady_clock::now();

    auto parent_span = t.tracer->StartSpan("request", {{"request.seq", seq}});
    trace_api::Scope parent_scope(parent_span);

    // work: a tiny, deterministic-but-variable amount of simulated cost.
    auto work_span = t.tracer->StartSpan("work", {{"request.seq", seq}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1 + seq % 4));
    work_span->End();

    // respond: write the reply line.
    auto respond_span = t.tracer->StartSpan("respond", {{"request.seq", seq}});
    const std::string reply = "ok " + std::to_string(seq) + "\n";
    const bool wrote = write_all(fd, reply);
    if (!wrote) {
        respond_span->SetStatus(trace_api::StatusCode::kError, "write reply failed");
    }
    respond_span->End();
    parent_span->End();

    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    t.requests_total->Add(1);
    // Histogram::Record(T) alone is an ABI-v2-only overload; this build is
    // pinned to ABI v1 (matching the rest of the conan cache), so an
    // explicit empty Context is required to select an ABI-v1 overload.
    t.request_duration->Record(elapsed_ms, opentelemetry::context::Context{});

    return wrote;
}

void serve_conn(const telemetry::Handle& t, int fd) {
    LineReader reader(fd);
    std::string line;
    while (reader.read_line(line)) {
        const std::string_view trimmed = trim(line);
        if (trimmed.empty()) {
            continue;
        }
        std::int64_t seq = 0;
        if (!parse_i64(trimmed, seq)) {
            if (!write_all(fd, "err bad-request\n")) {
                break;
            }
            continue;
        }
        if (!handle_request(t, fd, seq)) {
            std::println(stderr, "app: request error: write reply failed");
            break;
        }
    }
    ::close(fd);
}

// Event unifies the two sources serve waits on -- an incoming connection
// and a shutdown signal -- into the single queue a Go select over
// sigCh/connCh, or the Rust port's mpsc::channel over its Event enum, would
// use.
struct Event {
    enum class Kind { Signal, Conn } kind;
    std::string signal_name;
    int conn_fd = -1;
};

// EventChannel is a minimal thread-safe queue: the signal-wait thread and
// the accept thread both push onto it, the main loop pops and dispatches.
class EventChannel {
public:
    void push(Event e) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            queue_.push_back(std::move(e));
        }
        cv_.notify_one();
    }

    Event pop() {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this] { return !queue_.empty(); });
        Event e = std::move(queue_.front());
        queue_.pop_front();
        return e;
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Event> queue_;
};

int run_serve(const std::vector<std::string>& args) {
    std::string port;
    std::string endpoint = "http://localhost:4318";
    if (const char* env = std::getenv("OTEL_ENDPOINT"); env != nullptr && *env != '\0') {
        endpoint = env;
    }

    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--port") {
            if (++i >= args.size()) {
                usage();
            }
            port = args[i];
        } else if (args[i] == "--otel-endpoint") {
            if (++i >= args.size()) {
                usage();
            }
            endpoint = args[i];
        } else {
            usage();
        }
    }
    if (port.empty()) {
        usage();
    }

    std::uint16_t port_num = 0;
    if (!parse_port(port, port_num)) {
        die("listen: invalid port '" + port + "'");
    }

    // A shell running "app serve ... &" as a background job (the exact
    // pattern demo.sh/verify.lua use) sets SIGINT (and SIGQUIT) to SIG_IGN
    // in the child before exec -- standard POSIX shell behavior, inherited
    // across exec. Blocking + sigwait() alone does not override an
    // inherited "ignore" disposition; installing a real handler does, which
    // is exactly what Go's signal.Notify and Rust's signal-hook do under
    // the hood. The handler itself never runs (the signal stays blocked
    // below, so sigwait() -- not the handler -- is what actually consumes
    // it); this call exists purely to force the disposition away from
    // SIG_IGN.
    struct sigaction sa {};
    sa.sa_handler = [](int) {};
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

    // Block SIGINT/SIGTERM in this thread BEFORE spawning (or triggering
    // the SDK to spawn) any others: every thread created from here on --
    // including the BatchSpanProcessor/PeriodicExportingMetricReader
    // background threads telemetry::init() starts -- inherits the blocked
    // mask, so only the dedicated sigwait() thread below ever consumes
    // them. Doing this BEFORE telemetry::init() matters: those SDK threads
    // would otherwise start with the signal unblocked and the kernel could
    // deliver SIGINT to one of them instead of the sigwait() thread.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (const int rc = ::pthread_sigmask(SIG_BLOCK, &mask, nullptr); rc != 0) {
        die("sigmask: " + std::string(std::strerror(rc)));
    }

    telemetry::Handle t = telemetry::init(endpoint);

    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        die("listen: " + std::string(std::strerror(errno)));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_num);
    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        die("listen: invalid address 127.0.0.1");
    }
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        die("listen: " + std::string(std::strerror(errno)));
    }
    if (::listen(listen_fd, 16) < 0) {
        die("listen: " + std::string(std::strerror(errno)));
    }

    EventChannel channel;

    // Signal watcher: sigwait() blocks (in this thread only) until one of
    // the blocked signals is pending, then forwards it onto the shared
    // event queue -- the same role Go's signal.Notify channel and the
    // Rust port's signal-hook thread play.
    std::thread signal_thread([&channel, mask] {
        int sig = 0;
        if (::sigwait(&mask, &sig) == 0) {
            const char* name = sig == SIGINT ? "SIGINT" : sig == SIGTERM ? "SIGTERM" : "signal";
            channel.push(Event{Event::Kind::Signal, name, -1});
        }
    });
    signal_thread.detach();

    // Accept loop: forwards each accepted connection onto the same event
    // queue. The process exits by returning all the way out of main, not
    // by joining this thread -- same lifecycle as Go's accept goroutine
    // (cut short by os.Exit) and the Rust port's accept thread (never
    // joined).
    std::thread accept_thread([&channel, listen_fd] {
        for (;;) {
            const int fd = ::accept(listen_fd, nullptr, nullptr);
            if (fd < 0) {
                return; // listener closed (or otherwise gone) during shutdown
            }
            channel.push(Event{Event::Kind::Conn, "", fd});
        }
    });
    accept_thread.detach();

    std::println("app: serve listening on 127.0.0.1:{}", port);
    std::fflush(stdout);

    for (;;) {
        Event e = channel.pop();
        if (e.kind == Event::Kind::Signal) {
            std::println("app: shutting down ({})", e.signal_name);
            std::fflush(stdout);
            const bool ok = t.flush_and_shutdown(std::chrono::milliseconds(5000));
            if (!ok) {
                std::println(stderr, "app: telemetry shutdown: one or more providers failed to flush/shutdown");
            }
            return 0;
        }
        serve_conn(t, e.conn_fd);
    }
}

// ------------------------------------------------------------------- drive

// run_drive is a plain, untraced TCP client: it generates the load that
// makes serve's telemetry interesting, but emits none of its own -- the
// service under observation is serve, not drive.
int run_drive(const std::vector<std::string>& args) {
    std::int64_t n = 0;
    std::string addr_str = "127.0.0.1:8080";

    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--n") {
            if (++i >= args.size()) {
                usage();
            }
            if (!parse_i64(args[i], n)) {
                die("--n: invalid integer '" + args[i] + "'");
            }
        } else if (args[i] == "--addr") {
            if (++i >= args.size()) {
                usage();
            }
            addr_str = args[i];
        } else {
            usage();
        }
    }
    if (n <= 0) {
        usage();
    }

    const auto colon = addr_str.rfind(':');
    if (colon == std::string::npos) {
        die("dial " + addr_str + ": address must be HOST:PORT");
    }
    const std::string host = addr_str.substr(0, colon);
    std::uint16_t port_num = 0;
    if (!parse_port(addr_str.substr(colon + 1), port_num)) {
        die("dial " + addr_str + ": invalid port");
    }

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        die("dial " + addr_str + ": " + std::strerror(errno));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_num);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        die("dial " + addr_str + ": invalid host '" + host + "'");
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        const std::string msg = "dial " + addr_str + ": " + std::strerror(errno);
        ::close(fd);
        die(msg);
    }

    LineReader reader(fd);
    std::int64_t ok = 0;
    for (std::int64_t seq = 1; seq <= n; ++seq) {
        const std::string req = std::to_string(seq) + "\n";
        if (!write_all(fd, req)) {
            const std::string msg = "send request " + std::to_string(seq) + ": " + std::strerror(errno);
            ::close(fd);
            die(msg);
        }
        std::string line;
        if (!reader.read_line(line)) {
            break; // EOF: server closed the connection early.
        }
        if (line.starts_with("ok ")) {
            ++ok;
        }
    }
    ::close(fd);

    std::println("app: drive sent {}/{} ok", ok, n);
    std::fflush(stdout);
    return ok == n ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
    }
    const std::string_view subcommand = argv[1];
    const std::vector<std::string> args(argv + 2, argv + argc);

    int code = 0;
    if (subcommand == "serve") {
        code = run_serve(args);
    } else if (subcommand == "drive") {
        code = run_drive(args);
    } else {
        usage();
    }
    return code;
}
