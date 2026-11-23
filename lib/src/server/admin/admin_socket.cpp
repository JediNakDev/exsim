#include "server/admin/admin_socket.hpp"

#include "common/clocks.hpp"
#include "server/admin/core_admin.hpp"

#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

namespace exsim::server {

namespace {
constexpr std::size_t kReadBytes = 512;
}  // namespace

AdminSocket::~AdminSocket() {
    // The path may have been removed and reused since open(); leave another file alone.
    struct stat info {};
    if (listener_.is_open() && ::lstat(path_.c_str(), &info) == 0 && info.st_dev == device_ && info.st_ino == inode_)
        ::unlink(path_.c_str());
}

std::string AdminSocket::open(std::string path) {
    if (const net::ErrorCode error = net::open_unix_listener(path, 8, listener_); error != 0) {
        return "admin socket " + path + ": " + std::strerror(error) +
               (error == EADDRINUSE ? " (remove the file if no exchange is using it)" : "");
    }
    struct stat info {};
    if (::lstat(path.c_str(), &info) == 0) {
        device_ = info.st_dev;
        inode_ = info.st_ino;
    }
    path_ = std::move(path);
    // A command never grows past this before it is answered, so reading does not allocate.
    command_.reserve(kMaxCommandBytes + kReadBytes);
    return {};
}

std::string AdminSocket::start(ServerRuntime& runtime) {
    runtime_ = &runtime;
    if (const net::ErrorCode error = runtime.watch_readable_fd(listener_.fd(), on_readable, this); error != 0)
        return "admin socket " + path_ + ": " + std::strerror(error);
    return {};
}

void AdminSocket::close_stalled_client(std::uint64_t now_ns) noexcept {
    if (client_.is_open() && now_ns >= client_deadline_ns_) reply_and_close("ERR no complete command within 5 s\n");
}

void AdminSocket::on_readable(void* context) noexcept {
    auto& self = *static_cast<AdminSocket*>(context);
    if (self.client_.is_open()) {
        self.read_command();
        return;
    }
    net::Ipv4Endpoint unused;
    // EAGAIN, or a client that already left: wait for the next one.
    if (net::accept_connection(self.listener_.fd(), self.client_, unused) != 0) return;
    self.client_deadline_ns_ = common::monotonic_ns() + kCommandTimeoutNs;
    self.watch(self.client_.fd());
}

// One recv per readable event, like every other socket on the level-triggered poller.
void AdminSocket::read_command() noexcept {
    char buffer[kReadBytes];
    const ssize_t count = net::receive_some(client_.fd(), buffer, sizeof buffer);
    if (count < 0 && net::is_would_block(errno)) return;
    if (count < 0 || (count == 0 && command_.empty())) {  // an error, or the client left without a command
        reply_and_close({});
        return;
    }
    command_.append(buffer, static_cast<std::size_t>(count));
    const std::size_t newline = command_.find('\n');
    // End of input also ends the line, for a client that sends no newline.
    const std::size_t length = newline == std::string::npos ? command_.size() : newline;
    if (length > kMaxCommandBytes) {
        reply_and_close("ERR command longer than 4096 bytes\n");
        return;
    }
    if (newline == std::string::npos && count > 0) return;  // the rest of the line is still coming
    command_.resize(length);
    if (command_.ends_with('\r')) command_.pop_back();
    std::string reply;
    try {
        CoreAdmin(runtime_->core()).execute_line(command_, reply);
    } catch (...) {
        reply_and_close("ERR internal error\n");
        return;
    }
    reply_and_close(reply);
}

// A reply is a few KB to a local client, so a short blocking wait is simpler than a write queue.
void AdminSocket::reply_and_close(std::string_view reply) noexcept {
    const std::uint64_t deadline = common::monotonic_ns() + 1'000'000'000;
    while (!reply.empty()) {
        const ssize_t sent = net::send_some(client_.fd(), reply.data(), reply.size());
        if (sent > 0) {
            reply.remove_prefix(static_cast<std::size_t>(sent));
            continue;
        }
        const std::uint64_t now = common::monotonic_ns();
        if ((sent < 0 && !net::is_would_block(errno)) || now >= deadline) break;
        pollfd event{client_.fd(), POLLOUT, 0};
        (void)::poll(&event, 1, static_cast<int>((deadline - now) / 1'000'000) + 1);
    }
    watch(listener_.fd());  // replaces the client's registration before its fd closes
    client_.close();
    command_.clear();
}

void AdminSocket::watch(int fd) noexcept {
    if (const net::ErrorCode error = runtime_->watch_readable_fd(fd, on_readable, this); error != 0) {
        // Nothing is watched now: admin commands stop, trading goes on.
        std::fprintf(stderr, "admin socket %s stopped: %s\n", path_.c_str(), std::strerror(error));
    }
}

}  // namespace exsim::server
