#ifndef EXSIM_TEST_SUPPORT_TCP_CLIENT_HPP
#define EXSIM_TEST_SUPPORT_TCP_CLIENT_HPP
#include <algorithm>
#include <net/socket.hpp>
#include <net/stream_buffers.hpp>
namespace exsim::net {
// Caller registers fd() for write readiness during connect and while output
// is pending, and read readiness when connected. Storage survives reconnects.
class TcpClient {
public:
    TcpClient(std::size_t receive_capacity = 65536, std::size_t send_capacity = 1048576)
        : receive_buffer_(receive_capacity), send_buffer_(send_capacity) {}
    ErrorCode connect(Ipv4Endpoint remote) noexcept;
    ErrorCode complete_connect() noexcept;
    void close() noexcept {
        socket_.close();
        receive_buffer_.clear();
        send_buffer_.clear();
        connecting_ = false;
        connected_ = false;
    }
    bool connecting() const noexcept { return connecting_; }
    bool connected() const noexcept { return connected_; }
    int fd() const noexcept { return socket_.fd(); }
    ReceiveBuffer& receive_buffer() noexcept { return receive_buffer_; }
    // Queues `bytes`; output that does not fit closes the client.
    bool send(std::span<const char> bytes) noexcept {
        if (!connected_) return false;
        const std::span<char> space = send_buffer_.reserve(bytes.size());
        if (space.size() != bytes.size()) {
            close();
            return false;
        }
        std::ranges::copy(bytes, space.begin());
        send_buffer_.commit(bytes.size());
        return true;
    }
    ReceiveResult receive() noexcept;
    FlushResult flush() noexcept;

private:
    Socket socket_;
    ReceiveBuffer receive_buffer_;
    SendBuffer send_buffer_;
    bool connecting_ = false;
    bool connected_ = false;
};
}  // namespace exsim::net
#endif
