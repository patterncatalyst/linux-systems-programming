// hello-syscall: print "pid <PID> on <sysname> <release> (<machine>)" via getpid(2) + uname(2).

#include <cerrno>
#include <cstdio>
#include <expected>
#include <print>
#include <system_error>

#include <sys/utsname.h>
#include <unistd.h>

namespace {

[[nodiscard]] std::expected<utsname, std::error_code> system_info() {
    utsname info{};
    if (::uname(&info) != 0) {
        return std::unexpected(std::error_code{errno, std::system_category()});
    }
    return info;
}

} // namespace

int main() {
    const auto info = system_info();
    if (!info) {
        std::println(stderr, "uname failed: {}", info.error().message());
        return 1;
    }
    std::println("pid {} on {} {} ({})", ::getpid(), info->sysname, info->release, info->machine);
    return 0;
}
