#include "server/runtime/itch_runtime.hpp"
#include "test/framework/test.hpp"

TEST_CASE(itch_runtime, counts_udp_send_failures_but_not_successes_or_disabled_feed) {
    using namespace exsim;
    server::ServerConfig config;
    std::string error;
    REQUIRE(server::load_config("config/exchange.conf", config, error));
    config.market_data.retransmit_port = 0;
    config.market_data.retransmit_buffer = 32;
    MatchingEngine engine(config.engine.engine);
    for (const auto& instrument : config.instruments) REQUIRE(engine.add_instrument(instrument) != 0);
    net::UdpReceiver receiver;
    REQUIRE_EQ(receiver.open(net::loopback_endpoint(0)), 0);
    net::Ipv4Endpoint destination{};
    REQUIRE_EQ(net::local_endpoint(receiver.fd(), destination), 0);
    for (int mode = 0; mode < 3; ++mode) {
        // Broadcast without SO_BROADCAST fails on both macOS and Linux.
        config.market_data.address = mode == 1 ? "255.255.255.255" : "127.0.0.1";
        config.market_data.port = mode == 2 ? 0 : destination.port;
        server::runtime::MarketDataRuntimeSettings settings;
        settings.max_held_packets = 4;
        net::Poller poller;
        REQUIRE_EQ(poller.open(), 0);
        server::runtime::ItchRuntime runtime(config, settings);
        REQUIRE(runtime.open(config, poller, 1).empty());
        REQUIRE(runtime.start(0, engine, 0) == md::PublishStatus::Ok);
        runtime.flush();
        if (mode == 1)
            CHECK_GT(runtime.send_failure_count(), 0u);
        else
            CHECK_EQ(runtime.send_failure_count(), 0u);
    }
}
