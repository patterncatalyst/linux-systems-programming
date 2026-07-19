#include "httpd.hpp"

#include <cerrno>
#include <cstring>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <utility>

#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include "procfs.hpp"

namespace sysagent {

namespace {

// Same RAII-fd idiom used throughout the book: never a naked close(2).
class UniqueFd {
  public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~UniqueFd() { reset(); }
    [[nodiscard]] int get() const { return fd_; }
    void reset() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

  private:
    int fd_ = -1;
};

void send_all(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        auto n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

std::string http_response(int status, std::string_view status_text, std::string_view content_type,
                           const std::string& body) {
    return std::format("HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\n"
                        "Connection: close\r\n\r\n{}",
                        status, status_text, content_type, body.size(), body);
}

void handle_client(int cfd, int interval_ms) {
    char buf[4096];
    auto n = ::recv(cfd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;
    buf[n] = '\0';
    std::string_view req(buf, static_cast<size_t>(n));

    // Only care about the request line: "METHOD PATH HTTP/1.1".
    auto line_end = req.find("\r\n");
    std::string_view line = line_end == std::string_view::npos ? req : req.substr(0, line_end);
    auto sp1 = line.find(' ');
    auto sp2 = sp1 == std::string_view::npos ? std::string_view::npos : line.find(' ', sp1 + 1);
    std::string_view method = sp1 == std::string_view::npos ? "" : line.substr(0, sp1);
    std::string_view path =
        (sp1 == std::string_view::npos || sp2 == std::string_view::npos)
            ? ""
            : line.substr(sp1 + 1, sp2 - sp1 - 1);

    if (method == "GET" && path == "/metrics") {
        auto snap = take_snapshot(interval_ms);
        if (snap) {
            send_all(cfd, http_response(200, "OK", "application/json", to_json(*snap)));
        } else {
            send_all(cfd, http_response(500, "Internal Server Error", "text/plain", snap.error()));
        }
    } else {
        send_all(cfd, http_response(404, "Not Found", "text/plain", "not found\n"));
    }
}

} // namespace

int serve(int port, int interval_ms) {
    UniqueFd listen_fd(::socket(AF_INET, SOCK_STREAM, 0));
    if (listen_fd.get() < 0) {
        std::println(stderr, "sysagent: socket: {}", std::strerror(errno));
        return 1;
    }
    int one = 1;
    ::setsockopt(listen_fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(listen_fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::println(stderr, "sysagent: bind: {}", std::strerror(errno));
        return 1;
    }
    if (::listen(listen_fd.get(), 16) != 0) {
        std::println(stderr, "sysagent: listen: {}", std::strerror(errno));
        return 1;
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    UniqueFd sig_fd(::signalfd(-1, &mask, SFD_NONBLOCK));

    std::println("sysagent: listening on 0.0.0.0:{}", port);

    pollfd fds[2] = {
        {.fd = listen_fd.get(), .events = POLLIN, .revents = 0},
        {.fd = sig_fd.get(), .events = POLLIN, .revents = 0},
    };

    for (;;) {
        int rc = ::poll(fds, 2, -1);
        if (rc < 0) {
            if (errno == EINTR) continue;
            std::println(stderr, "sysagent: poll: {}", std::strerror(errno));
            return 1;
        }
        if (fds[1].revents & POLLIN) {
            signalfd_siginfo info{};
            (void)::read(sig_fd.get(), &info, sizeof(info));
            std::println("sysagent: shutting down");
            return 0;
        }
        if (fds[0].revents & POLLIN) {
            UniqueFd cfd(::accept(listen_fd.get(), nullptr, nullptr));
            if (cfd.get() >= 0) handle_client(cfd.get(), interval_ms);
        }
    }
}

} // namespace sysagent
