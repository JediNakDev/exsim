#ifndef TEST_EXSIM_PROTOCOL_MOLDUDP64_READER_HPP
#define TEST_EXSIM_PROTOCOL_MOLDUDP64_READER_HPP
#include <protocol/moldudp64.hpp>
namespace exsim::protocol::moldudp64 {
class PacketReader {
    std::span<const char> bytes_{};
    std::size_t offset_ = 20;
    std::uint16_t remaining_ = 0;

public:
    Request header{};
    Status reset(std::span<const char> p) noexcept {
        bytes_ = {};
        remaining_ = 0;
        offset_ = 20;
        if (p.size() < 20) return Status::incomplete;
        decode(p.data(), 20, header);
        auto count = header.count == 65535 ? 0 : header.count;
        std::size_t pos = 20;
        for (unsigned i = 0; i < static_cast<unsigned>(count); ++i) {
            if (p.size() - pos < 2) return Status::incomplete;
            auto n = wire::load16(p.data() + pos);
            pos += 2;
            if (!n) return Status::invalid_length;
            if (n > p.size() - pos) return Status::incomplete;
            pos += n;
        }
        if (pos != p.size()) return Status::invalid_length;
        bytes_ = p;
        remaining_ = static_cast<std::uint16_t>(count);
        return Status::ok;
    }
    Status next(std::span<const char>& message) noexcept {
        if (!remaining_) return Status::end;
        auto n = wire::load16(bytes_.data() + offset_);
        offset_ += 2;
        message = bytes_.subspan(offset_, n);
        offset_ += n;
        --remaining_;
        return Status::ok;
    }
};
}

#endif  // TEST_EXSIM_PROTOCOL_MOLDUDP64_READER_HPP
