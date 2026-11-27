// One MDP TCP replay connection's input (login, then one recovery request),
// answered from a ring that holds packets 5 to 12 (1 to 4 have been evicted).
#include "test/fuzz/exchange.hpp"
#include "md/mdp_replay_session.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const exsim::server::MarketDataSection config = [] {
        exsim::server::MarketDataSection result;
        result.channel_id = 310;
        result.replay_username = "CME";
        result.replay_password = "exsim-replay";
        return result;
    }();
    static const exsim::md::MdpReplayRing ring = [] {
        exsim::md::MdpReplayRing result(8, 64);
        for (std::uint32_t sequence = 1; sequence <= 12; ++sequence)
            (void)result.append(sequence, std::array<char, 32>{'P'});
        return result;
    }();
    fuzz::Output output;
    exsim::md::MdpReplaySession<fuzz::Sink> session(config, ring);
    session.open(fuzz::Sink{&output}, 0);
    (void)session.on_data({reinterpret_cast<const char*>(data), size}, 0, fuzz::Clock{}.realtime_ns());
    return 0;
}
