// A MoldUDP64 retransmission request datagram, answered from a ring that
// holds sequences 5 to 12 (1 to 4 have been evicted), three per response.
#include "md/retransmission_server.hpp"

#include <array>

namespace {
struct Feed {
    exsim::md::RetransmissionRing ring{8};
    exsim::md::RetransmissionServer server{ring, {"EXSIM", 72, 1024}};
    Feed() {
        for (std::uint64_t sequence = 1; sequence <= 12; ++sequence)
            (void)ring.append(sequence, std::array<char, 14>{'S'});
    }
};
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static Feed feed;
    (void)feed.server.answer({reinterpret_cast<const char*>(data), size}, [](std::span<const char>) {});
    return 0;
}
