#ifndef EXSIM_SERVER_ADMIN_ADMIN_SOCKET_HPP
#define EXSIM_SERVER_ADMIN_ADMIN_SOCKET_HPP

// Runtime admin commands (core_admin.hpp) over a Unix domain socket, opened
// when the config's [exchange] section sets `admin_socket = PATH`:
//
//   echo 'phase AAPL halted' | nc -U PATH
//
// One command per connection: the client sends one line, gets the reply lines
// (the last is "OK" or "ERR <message>"), and the exchange closes the
// connection. End of input ends the reply; a body line may start with any
// symbol, including "ERR".
// The socket file is mode 0600, so only its owner can connect. That is the
// only access control, which is why there is no TCP admin port.
//
// Everything runs on the reactor thread through ServerRuntime's one watched
// fd: the listener until a client connects, then that client until its line
// is complete. Other clients wait in the listen backlog. Reads never block
// the loop, and a client that has not sent its line within 5 s is answered
// with an error, so it cannot hold up the next one. The reply is written with
// a 1 s deadline, so a local client that stops reading stalls trading for at
// most that long.

#include "net/socket.hpp"
#include "server/runtime/server_runtime.hpp"

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace exsim::server {

class AdminSocket {
public:
    static constexpr std::size_t kMaxCommandBytes = 4096;
    static constexpr std::uint64_t kCommandTimeoutNs = 5'000'000'000;

    AdminSocket() = default;
    AdminSocket(const AdminSocket&) = delete;
    AdminSocket& operator=(const AdminSocket&) = delete;
    // Removes the socket file if the path still names the one open() created.
    // It does not touch the runtime: declare the AdminSocket before the
    // runtime, so the runtime and its poller are gone first.
    ~AdminSocket();

    // Creates the socket file; an existing path is refused and left alone.
    // Call it before runtime.start(), so a taken path fails before a
    // listener opens, the journal is truncated or a feed packet goes out.
    // Returns "" or an error message.
    [[nodiscard]] std::string open(std::string path);
    // After runtime.start(): answers commands from now on. Returns "" or an error message.
    [[nodiscard]] std::string start(ServerRuntime& runtime);
    // Between polls: answers and drops a client that has not sent its line by
    // its deadline. It is late by up to one poll wait (1 ms in exsim_exchanged).
    void close_stalled_client(std::uint64_t now_ns) noexcept;

private:
    static void on_readable(void* context) noexcept;
    void read_command() noexcept;
    void reply_and_close(std::string_view reply) noexcept;
    void watch(int fd) noexcept;

    ServerRuntime* runtime_ = nullptr;
    std::string path_;
    dev_t device_ = 0;  // identity of the socket file open() created
    ino_t inode_ = 0;
    net::Socket listener_;
    net::Socket client_;
    std::uint64_t client_deadline_ns_ = 0;
    std::string command_;
};

}  // namespace exsim::server

#endif
