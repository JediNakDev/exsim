#ifndef TEST_EXSIM_PROTOCOL_FIX44_READER_HPP
#define TEST_EXSIM_PROTOCOL_FIX44_READER_HPP
#include <protocol/fix44.hpp>
namespace exsim::protocol::fix44 {
class FrameReader {
    std::span<char> buffer_;
    std::size_t used_ = 0;
    bool ready_ = false;

public:
    explicit FrameReader(std::span<char> b) noexcept : buffer_(b) {}
    void reset() noexcept {
        used_ = 0;
        ready_ = false;
    }
    Status feed(std::span<const char> input, std::size_t& consumed, std::span<const char>& message) noexcept {
        consumed = 0;
        if (ready_) reset();
        while (true) {
            std::size_t size = 0;
            auto status = frame_size(buffer_.first(used_), size, buffer_.size());
            if (status == Status::ok) {
                ready_ = true;
                message = buffer_.first(size);
                return status;
            }
            if (status != Status::incomplete) return status;
            if (consumed == input.size()) return status;
            if (used_ == buffer_.size()) return Status::full;
            buffer_[used_++] = input[consumed++];
        }
    }
};
}

#endif  // TEST_EXSIM_PROTOCOL_FIX44_READER_HPP
