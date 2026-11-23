#ifndef EXSIM_SERVER_ADMIN_ADMIN_SOCKET_HPP
#define EXSIM_SERVER_ADMIN_ADMIN_SOCKET_HPP

// Runtime admin commands (core_admin.hpp) over a Unix domain socket, opened by
// `exsim_exchanged --admin-socket PATH`:
//
//   echo 'phase AAPL halted' | nc -U PATH
//
// One command per connection: the client sends one line, gets the reply lines
// (ending in "OK" or "ERR <message>"), and the exchange closes the connection.
// The socket file is mode 0600, so only its owner can connect. That is the
// only access control, which is why there is no TCP admin port.
//
// Everything runs on the reactor thread through ServerRuntime's one watched
// fd: the listener until a client connects, then that client until its line
// is complete. Other clients wait in the listen backlog. Reads never block
// the loop. The reply is written with a 1 s deadline, so a local client that
// stops reading stalls trading for at most that long.

#include "net/socket.hpp"
#include "server/runtime/server_runtime.hpp"

#include <cstddef>
#include <string>

namespace exsim::server {

class AdminSocket {
public:
    static constexpr std::size_t kMaxCommandBytes = 4096;

    explicit AdminSocket(ServerRuntime& runtime) noexcept : runtime_(runtime) {}
    AdminSocket(const AdminSocket&) = delete;
    AdminSocket& operator=(const AdminSocket&) = delete;
    // Stops watching and removes the socket file.
    ~AdminSocket();

    // Creates the socket file; an existing path is refused and left alone.
    // Call it before runtime.start(), so a taken path fails before any
    // listener opens or feed packet goes out. Returns "" or an error message.
    [[nodiscard]] std::string open(std::string path);
    // After runtime.start(): answers commands from now on. Returns "" or an error message.
    [[nodiscard]] std::string start();

private:
    static void on_readable(void* context) noexcept;
    void read_command() noexcept;
    void reply_and_close(std::string_view reply) noexcept;
    void watch(int fd) noexcept;

    ServerRuntime& runtime_;
    std::string path_;
    net::Socket listener_;
    net::Socket client_;
    std::string command_;
};

}  // namespace exsim::server

#endif
