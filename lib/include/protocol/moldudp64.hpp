#ifndef EXSIM_PROTOCOL_MOLDUDP64_HPP
#define EXSIM_PROTOCOL_MOLDUDP64_HPP
// A full builder never partially appends a message: flush packet(), reset(),
// then retry.
#include "wire.hpp"
#include <algorithm>
namespace exsim::protocol::moldudp64 {
using protocol::Status;
struct Request {
    std::array<char, 10> session = wire::spaces<10>();
    std::uint64_t sequence{};
    std::uint16_t count{};
};
inline std::size_t encode(const Request& m, char* p, std::size_t n) noexcept {
    if (n < 20) return 0;
    for (unsigned i = 0; i < 10; ++i) p[i] = m.session[i];
    wire::store64(p + 10, m.sequence);
    wire::store16(p + 18, m.count);
    return 20;
}
inline Status decode(const char* p, std::size_t n, Request& m) noexcept {
    if (n < 20) return Status::incomplete;
    if (n != 20) return Status::invalid_length;
    for (unsigned i = 0; i < 10; ++i) m.session[i] = p[i];
    m.sequence = wire::load64(p + 10);
    m.count = wire::load16(p + 18);
    return Status::ok;
}
class PacketBuilder {
    std::span<char> buffer_;
    std::size_t used_ = 0;
    std::uint16_t count_ = 0;

public:
    explicit PacketBuilder(std::span<char> b, std::size_t max_payload = 1400) noexcept
        : buffer_(b.first(std::min(b.size(), max_payload))) {}
    bool reset(std::string_view session, std::uint64_t sequence, bool end = false) noexcept {
        used_ = 0;
        count_ = 0;
        if (buffer_.size() < 20 || session.size() > 10) return false;
        wire::alpha(buffer_.data(), 10, session);
        wire::store64(buffer_.data() + 10, sequence);
        wire::store16(buffer_.data() + 18, end ? 65535 : 0);
        used_ = 20;
        count_ = end ? 65535 : 0;
        return true;
    }
    Status append(std::span<const char> message) noexcept {
        if (!used_ || count_ >= 65534) return Status::full;
        if (message.empty() || message.size() > 65535) return Status::invalid_length;
        if (message.size() + 2 > buffer_.size() - used_) return Status::full;
        wire::store16(buffer_.data() + used_, static_cast<std::uint16_t>(message.size()));
        used_ += 2;
        std::copy_n(message.data(), message.size(), buffer_.data() + used_);
        used_ += message.size();
        wire::store16(buffer_.data() + 18, ++count_);
        return Status::ok;
    }
    std::span<const char> packet() const noexcept { return buffer_.first(used_); }
};
}  // namespace exsim::protocol::moldudp64
#endif
