#include "test/exsim/support/clients/tcp_client.hpp"

#include <arpa/inet.h>
namespace exsim::net {
namespace {
// Starts a non-blocking connect: 0 when it completed immediately, EINPROGRESS
// when it completes later (wait for write readiness), otherwise the error.
ErrorCode start_connect(int fd, Ipv4Endpoint remote) noexcept {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(remote.address);
    address.sin_port = htons(remote.port);
    return ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0 ? errno : 0;
}
// Outcome of an in-progress non-blocking connect: 0 = connected, EINPROGRESS
// = not finished yet, otherwise the connect error (e.g. ECONNREFUSED).
ErrorCode connect_result(int fd) noexcept {
    int error = 0;
    socklen_t length = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0) return errno;
    if (error) return error;
    sockaddr_in peer{};
    length = sizeof(peer);
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &length) == 0) return 0;
    return errno == ENOTCONN ? EINPROGRESS : errno;
}
}  // namespace
ErrorCode TcpClient::connect(Ipv4Endpoint remote) noexcept {
    close();
    Socket socket;
    int error = open_tcp_socket(socket);
    if (error) return error;
    if ((error = set_tcp_no_delay(socket.fd(), true))) return error;
    error = start_connect(socket.fd(), remote);
    if (error && error != EINPROGRESS) return error;
    socket_ = std::move(socket);
    connecting_ = error == EINPROGRESS;
    connected_ = !connecting_;
    return error;
}
ErrorCode TcpClient::complete_connect() noexcept {
    if (connected_) return 0;
    if (!connecting_) return ENOTCONN;
    const int error = connect_result(fd());
    if (error == EINPROGRESS) return error;
    if (error) {
        close();
        return error;
    }
    connecting_ = false;
    connected_ = true;
    return 0;
}
ReceiveResult TcpClient::receive() noexcept {
    if (!connected_) return {ReceiveStatus::Error, ENOTCONN, 0};
    const auto received = receive_buffer_.receive_from(socket_.fd());
    // Keep final received bytes available until the caller consumes them.
    if (received.status == ReceiveStatus::PeerClosed || received.status == ReceiveStatus::Error ||
        received.status == ReceiveStatus::BufferFull) {
        socket_.close();
        connected_ = false;
    }
    return received;
}
FlushResult TcpClient::flush() noexcept {
    if (!connected_) return {FlushStatus::Error, ENOTCONN, 0};
    const auto flushed = send_buffer_.flush_to(socket_.fd());
    if (flushed.status == FlushStatus::Error) close();
    return flushed;
}
}  // namespace exsim::net
