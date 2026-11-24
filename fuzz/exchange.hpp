#ifndef EXSIM_FUZZ_EXCHANGE_HPP
#define EXSIM_FUZZ_EXCHANGE_HPP

// The production exchange core with all three order-entry gateways, for the
// gateway and admin harnesses. A harness builds a new one per input, so every
// input replays exactly: parse_config reads kConfig, the clock is fixed, and
// replies go to a 64 KiB buffer that is never drained (a full one closes the
// connection, as a full TCP send buffer does). The gateway seeds in fuzz/seeds
// log in as these users at this clock; change either and they stop at login.

#include "server/admin/core_admin.hpp"
#include "server/runtime/gateway_sessions.hpp"
#include "server/runtime/order_entry_gateways.hpp"

#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fuzz {

inline constexpr std::string_view kConfig = R"(
[market_data]
port = 16000
[engine]
max_instruments = 2
max_orders = 64
max_tokens_per_session = 64
store_messages_per_session = 256
store_bytes_per_session = 65536
[instrument ES]
cme_security_id = 800001
tick_size = 0.25
min_price = 1
max_price = 1000
reference_price = 150
initial_phase = continuous
[user OUCH]
password = ouch
protocol = ouch42
session_id = 1
[user FIX]
password = fix
protocol = fix44
session_id = 2
[user ILINK]
protocol = ilink3
session_id = 3
ilink_access_key = ACCESS-1
ilink_secret_key = AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=
ilink_session = AAA
ilink_firm = EXSIM
ilink_party_id = 1001
)";

// 10:00 local time on 2023-11-14 (UTC epoch 1'700'000'000 s at midnight).
struct Clock {
    exsim::Timestamp day_ns() const noexcept { return 36'000'000'000'000; }
    std::uint64_t realtime_ns() const noexcept { return 1'700'000'000'000'000'000 + day_ns(); }
};

struct NoMarketData {
    void on_events(std::span<const exsim::Event>) noexcept {}
    void flush() noexcept {}
};

struct Output {
    std::vector<char> bytes = std::vector<char>(std::size_t{1} << 16);
    std::size_t used = 0;
};

struct Sink {
    Output* output = nullptr;
    char* prepare(std::size_t size) noexcept {
        return size <= output->bytes.size() - output->used ? output->bytes.data() + output->used : nullptr;
    }
    void commit(std::size_t size) noexcept { output->used += size; }
};

using OuchGateway = exsim::server::runtime::BasicOuchGateway<Sink>;
using FixGateway = exsim::server::runtime::BasicFixGateway<Sink>;
using IlinkGateway = exsim::server::runtime::BasicIlinkGateway<Sink>;
using Sessions = exsim::server::runtime::GatewaySessions<OuchGateway, FixGateway, IlinkGateway>;
using Core = exsim::server::ExchangeCore<Sessions, NoMarketData, Clock>;

inline exsim::server::ServerConfig parsed_config() {
    exsim::server::ServerConfig config;
    std::string error;
    if (!exsim::server::parse_config(kConfig, config, error)) std::abort();
    return config;
}

struct Exchange {
    exsim::server::ServerConfig config = parsed_config();
    Sessions sessions{config};
    NoMarketData market_data;
    Clock clock;
    Core core{config, sessions, market_data, clock};
    Output output;

    // One connection that receives `bytes` in a single read, then closes:
    // what the order-entry endpoint calls on the gateway.
    template <class Gateway>
    void receive(std::span<const char> bytes) {
        typename Gateway::template Connection<Core> connection(sessions.user_directory<Gateway>());
        connection.open(Sink{&output}, 1, core);
        connection.on_data(bytes, core);
        connection.on_closed(core);
    }
};

}  // namespace fuzz

#endif
