#ifndef TEST_EXSIM_PROTOCOL_SOUPBINTCP_CLIENT_CODEC_HPP
#define TEST_EXSIM_PROTOCOL_SOUPBINTCP_CLIENT_CODEC_HPP
// Login reply decoders only a test client needs; the exchange only encodes these replies.
#include <protocol/soupbintcp.hpp>
namespace exsim::protocol::soupbintcp {
inline Status decode(const char* p, std::size_t n, LoginAccepted& m) noexcept {
    Packet pkt;
    auto s = decode(p, n, pkt);
    if (s != Status::ok) return s;
    if (pkt.type != 'A') return Status::invalid_type;
    if (!wire::numeric({p + 13, 20}, m.sequence)) return Status::invalid_field;
    for (unsigned i = 0; i < 10; ++i) m.session[i] = p[3 + i];
    return Status::ok;
}
inline Status decode(const char* p, std::size_t n, LoginRejected& m) noexcept {
    Packet packet;
    auto status = decode(p, n, packet);
    if (status != Status::ok) return status;
    if (packet.type != 'J') return Status::invalid_type;
    m.reason = packet.payload[0];
    return Status::ok;
}
}  // namespace exsim::protocol::soupbintcp
#endif
