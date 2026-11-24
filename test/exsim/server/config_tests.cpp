#include "md/itch_publisher.hpp"
#include "server/config.hpp"
#include "common/price_text.hpp"
#include "../../framework/test.hpp"

#include <algorithm>
#include <string>

using namespace exsim;
using namespace exsim::server;

namespace {
const char* kMinimal = R"(
[market_data]
port = 16000

[instrument AAPL]
tick_size = 0.01
min_price = 0.01
max_price = 1000
reference_price = 150.25

[user ALICE]
password = pw
session_id = 1
)";
}

TEST_CASE(server_config_mdp, fixture_and_cme_identity) {
    ServerConfig config;
    std::string error;
    REQUIRE(load_config("test/fixtures/mdp3.conf", config, error));
    CHECK_EQ(config.market_data.protocol, MarketDataProtocol::Mdp3);
    CHECK_EQ(config.market_data.channel_id, 310);
    CHECK_EQ(config.market_data.book_depth, 10);
    CHECK_EQ(config.instruments[0].cme_security_id, 800001);
    CHECK_EQ(std::string_view(config.instruments[0].cme_symbol.data(), 19), "ES-SIMULATED-FUTURE");
}

TEST_CASE(server_config_mdp, rejects_invalid_profile_without_truncating) {
    const std::string prefix = R"(
[market_data]
protocol = mdp3
port = 19001
snapshot_port = 19002
instrument_port = 19003
replay_username = CME
replay_password = secret
)";
    const std::string instrument = R"(
[instrument ES]
cme_security_id = 800001
min_price = 1
max_price = 100
reference_price = 10
)";
    for (const auto extra : {"book_depth=11\n", "channel_id=32768\n", "market_segment_id=255\n",
                             "snapshot_port=19001\n", "max_payload=1399\n", "replay_password=\n"}) {
        ServerConfig config;
        std::string error;
        CHECK(!parse_config(prefix + extra + instrument, config, error));
    }
    for (const auto extra : {"cme_security_id=2147483647\n", "cme_security_id=0\n",
                             "cme_symbol=123456789012345678901\n", "tick_size=0.00001\n"}) {
        ServerConfig config;
        std::string error;
        CHECK(!parse_config(prefix + instrument + extra, config, error));
    }
    ServerConfig config;
    std::string error;
    CHECK(!parse_config(prefix + instrument + "[instrument NQ]\ncme_security_id=800001\n", config, error));
}

TEST_CASE(server_config_mdp, rejects_keys_of_the_other_market_data_protocol) {
    const std::string mdp =
        "[market_data]\nprotocol = mdp3\nport = 19001\nsnapshot_port = 19002\ninstrument_port = 19003\n"
        "replay_username = CME\nreplay_password = secret\nmarket_segment_id = 2\n";
    const std::string instrument_and_user =
        "[instrument ES]\ncme_security_id = 800001\nmin_price = 1\nmax_price = 100\nreference_price = 10\n"
        "[user ALICE]\nsession_id = 1\n";
    ServerConfig config;
    std::string error;
    REQUIRE(parse_config(mdp + instrument_and_user, config, error));
    for (const auto itch_key : {"session = EXSIM\n", "retransmit_buffer = 16\n", "noii_interval_ms = 5\n"}) {
        CHECK(!parse_config(mdp + itch_key + instrument_and_user, config, error));
        CHECK(error.find("ignored by the selected protocol") != std::string::npos);
    }
    for (const auto mdp_key :
         {"channel_id = 7\n", "book_depth = 5\n", "snapshot_port = 19002\n", "replay_password = x\n"}) {
        CHECK(!parse_config(std::string(kMinimal) + "[market_data]\nport = 19001\n" + mdp_key, config, error));
        CHECK(error.find("ignored by the selected protocol") != std::string::npos);
    }
    CHECK(parse_config(std::string(kMinimal) + "[market_data]\nport = 19001\nsession = ABC\nmarket_segment_id = 2\n",
                       config, error));
}

TEST_CASE(server_config_ilink, current_schema_fixture_and_missing_credentials) {
    ServerConfig config;
    std::string error;
    REQUIRE(load_config("config/cme.conf", config, error));
    REQUIRE_EQ(config.users.back().protocol, Protocol::Ilink3);
    CHECK_EQ(config.users.back().ilink_party_id, 1001u);
    CHECK_EQ(config.users.back().ilink_session, "AAA");
    auto text = std::string(kMinimal);
    const auto instrument = text.find("[instrument AAPL]\n") + std::string_view("[instrument AAPL]\n").size();
    text.insert(instrument, "cme_security_id=800001\n");
    text += "\n[user ILINK]\nprotocol=ilink3\nsession_id=2\n";
    CHECK(!parse_config(text, config, error));
    CHECK(error.find("iLink user requires") != std::string::npos);
}

// Finding 106: no gateway or report reads an iLink account, so the key is gone.
TEST_CASE(server_config_ilink, account_is_not_a_config_key) {
    ServerConfig config;
    std::string error;
    std::string text(kMinimal);
    const auto instrument = text.find("[instrument AAPL]\n") + std::string_view("[instrument AAPL]\n").size();
    text.insert(instrument, "cme_security_id=800001\n");
    text += "\n[user ILINK]\nprotocol=ilink3\nsession_id=2\nilink_access_key=ACCESS-1\nilink_secret_key=" +
            std::string(43, 'A') + "=\nilink_session=AAA\nilink_firm=EXSIM\nilink_party_id=1001\n";
    REQUIRE(parse_config(text, config, error));
    text += "ilink_account=SIM-ACCOUNT\n";
    CHECK(!parse_config(text, config, error));
    CHECK(error.find("ilink_account") != std::string::npos);
}

TEST_CASE(server_config, prices) {
    Price p = 0;
    CHECK(parse_price("150.25", p));
    CHECK_EQ(p, 1'502'500);
    CHECK(parse_price("0.0001", p));
    CHECK_EQ(p, 1);
    CHECK(parse_price("7", p));
    CHECK_EQ(p, 70'000);
    CHECK(parse_price("-1.5", p));
    CHECK_EQ(p, -15'000);
    CHECK(parse_price(".5", p));
    CHECK_EQ(p, 5'000);
    CHECK(!parse_price("1.00001", p));
    CHECK(!parse_price("1.", p));
    CHECK(!parse_price("", p));
    CHECK(!parse_price("-", p));
    CHECK(!parse_price("1e3", p));
    CHECK(!parse_price("99999999999999999999", p));
    CHECK(!parse_price("922337203685478", p));  // * 10^4 overflows int64
    CHECK(format_price(1'502'500, 0) == "150.25");
    CHECK(format_price(70'000, 0) == "7");
    CHECK(format_price(1, 0) == "0.0001");
    CHECK(format_price(-15'000, 0) == "-1.5");
    CHECK(format_price(0, 0) == "0");
}

TEST_CASE(server_config, price_int64_limits_and_transactional_overflow) {
    Price price = 0;
    REQUIRE(parse_price("922337203685477.5807", price));
    CHECK_EQ(price, INT64_MAX);
    REQUIRE(parse_price("-922337203685477.5808", price));
    CHECK_EQ(price, INT64_MIN);
    CHECK_EQ(format_price(price, 0), "-922337203685477.5808");
    for (const auto text :
         {"922337203685477.5808", "-922337203685477.5809", "-922337203685477.587.", "922337203685478", "--1"}) {
        price = 123;
        CHECK(!parse_price(text, price));
        CHECK_EQ(price, 123);
    }
}

TEST_CASE(server_config, engine_limit_errors_identify_the_source_line) {
    for (const auto key : {"max_instruments = 65536", "max_orders = 4294967296"}) {
        ServerConfig config;
        std::string error;
        CHECK(!parse_config(std::string("[engine]\n") + key + "\n" + kMinimal, config, error));
        CHECK(error.starts_with("2:"));
    }
}

TEST_CASE(server_config, minimal_uses_defaults) {
    ServerConfig cfg;
    std::string err;
    REQUIRE(parse_config(kMinimal, cfg, err));
    CHECK(err.empty());
    REQUIRE_EQ(cfg.instruments.size(), 1u);
    CHECK(cfg.instruments[0].symbol.view() == "AAPL");
    CHECK_EQ(cfg.instruments[0].tick_size, 100);
    CHECK_EQ(cfg.instruments[0].max_price, 10'000'000);
    CHECK_EQ(cfg.instruments[0].reference_price, 1'502'500);
    REQUIRE_EQ(cfg.users.size(), 1u);
    CHECK(cfg.users[0].protocol == Protocol::Ouch42);
    CHECK(cfg.users[0].cancel_on_disconnect);
    CHECK(cfg.exchange.name == "EXSIM");
    CHECK(cfg.users[0].username == "ALICE");
}

// Large capacity is an explicit config choice: the defaults stay small
// (documented in docs/running.md).
TEST_CASE(server_config, defaults_are_small) {
    ServerConfig cfg;
    std::string err;
    REQUIRE(parse_config(kMinimal, cfg, err));
    CHECK_EQ(cfg.engine.engine.max_orders, std::size_t{1} << 16);
    CHECK_EQ(cfg.engine.max_tokens_per_session, 1u << 16);
    CHECK_EQ(cfg.engine.store_messages_per_session, std::size_t{1} << 16);
    CHECK_EQ(cfg.engine.store_bytes_per_session, std::size_t{8} << 20);
    CHECK_EQ(cfg.market_data.retransmit_buffer, 1u << 16);
}

TEST_CASE(server_config, example_file_parses) {
    ServerConfig cfg;
    std::string error;
    REQUIRE(load_config("config/exchange.conf", cfg, error));
    CHECK_EQ(cfg.instruments.size(), 1u);
    CHECK_EQ(cfg.users.size(), 2u);
    CHECK_EQ(cfg.exchange.ouch_port, 15000);
    CHECK_EQ(cfg.exchange.fix_port, 0);
    CHECK_EQ(cfg.market_data.port, 16000);
    CHECK(cfg.instruments[0].initial_phase == Phase::Continuous);
}

TEST_CASE(server_config, bot_sections_are_rejected) {
    ServerConfig cfg;
    std::string error;
    CHECK(!parse_config("[bot maker]\ntype=market_maker\n", cfg, error));
    CHECK(error.find("unknown section") != std::string::npos);
}

TEST_CASE(server_config, errors_carry_line_numbers) {
    auto error_of = [](const std::string& text) {
        ServerConfig cfg;
        std::string err;
        CHECK(!parse_config(text, cfg, err));
        return err;
    };
    CHECK(error_of("tick_size = 1\n").rfind("1: ", 0) == 0);
    CHECK(error_of("\n[bogus]\n").rfind("2: unknown section", 0) == 0);
    CHECK(error_of("[instrument X]\nfoo = 1\n").rfind("2: unknown key", 0) == 0);
    CHECK(error_of("[instrument X]\ntick_size = abc\n").rfind("2: invalid value", 0) == 0);
    CHECK(error_of("[instrument TOOLONGSYM]\n").rfind("1: ", 0) == 0);
    CHECK(error_of("[exchange\n").rfind("1: unterminated", 0) == 0);
}

TEST_CASE(server_config, semantic_validation) {
    auto fails = [](const std::string& extra) {
        ServerConfig cfg;
        std::string err;
        return !parse_config(std::string(kMinimal) + extra, cfg, err);
    };
    CHECK(fails("[user ALICE]\nsession_id = 2\n"));        // duplicate user
    CHECK(fails("[user BOB]\nsession_id = 1\n"));          // duplicate session
    CHECK(fails("[user BOB]\npassword = x\n"));            // missing session_id
    CHECK(fails("[user TOOLONGNAME]\nsession_id = 5\n"));  // OUCH username > 6
    CHECK(!fails("[user TOOLONGNAME]\nsession_id = 5\nprotocol = fix44\n"));
    CHECK(fails("[instrument AAPL]\n"));                       // duplicate instrument
    CHECK(fails("[instrument B]\nmin_price = 0.015\n"));       // off-tick band
    CHECK(fails("[instrument B]\nreference_price = 2000\n"));  // outside default band
    CHECK(fails("[exchange]\nsoup_session = ELEVENCHARS\n"));
    {
        ServerConfig cfg;
        std::string err;
        CHECK(!parse_config("[user A]\nsession_id = 1\n", cfg, err));  // no instruments
    }
}

namespace {
ServerConfig parse_ok(const std::string& text) {
    ServerConfig cfg;
    std::string err;
    const bool ok = parse_config(text, cfg, err);
    if (!ok) testfw::report_failure(__FILE__, __LINE__, err);
    REQUIRE(ok);
    return cfg;
}

}  // namespace

TEST_CASE(server_config, trading_day_admin_socket_and_journal_keys) {
    ServerConfig cfg = parse_ok(std::string(kMinimal) + R"(
[exchange]
admin_socket = run/admin.sock
trading_day_start = 09:30
trading_day_end = 16:00:00.5

[journal]
path = /tmp/x.journal
on_failure = continue
flush_interval_ms = 0
)");
    CHECK_EQ(cfg.exchange.trading_day_start_ns, (9 * 3600 + 30 * 60) * 1'000'000'000ull);
    CHECK_EQ(cfg.exchange.trading_day_end_ns, 16 * 3600 * 1'000'000'000ull + 500'000'000ull);
    CHECK_EQ(cfg.exchange.admin_socket, "run/admin.sock");
    CHECK(parse_ok(kMinimal).exchange.admin_socket.empty());
    CHECK(cfg.journal_failure_policy == JournalFailurePolicy::Continue);
    CHECK_EQ(cfg.journal_flush_interval_ms, 0u);

    const auto fails = [](const std::string& extra) {
        ServerConfig parsed;
        std::string err;
        return !parse_config(std::string(kMinimal) + extra, parsed, err);
    };
    CHECK(fails("[exchange]\ntrading_day_start = 16:00\ntrading_day_end = 09:30\n"));
    {
        // A day that crosses midnight is rejected at startup with a message
        // that says so; the trading day must lie within one calendar day.
        ServerConfig parsed;
        std::string err;
        CHECK(!parse_config(std::string(kMinimal) + "[exchange]\ntrading_day_start = 22:00\ntrading_day_end = 02:00\n",
                            parsed, err));
        CHECK(err.find("cross midnight") != std::string::npos);
        CHECK(err.find("22:00:00") != std::string::npos);
    }
    CHECK(fails("[exchange]\ntrading_day_start = 10:00\ntrading_day_end = 10:00\n"));
    CHECK(fails("[exchange]\ntrading_day_end = 24:01\n"));
    CHECK(fails("[exchange]\ntrading_day_start = 9h\n"));
    CHECK(fails("[journal]\non_failure = retry\n"));
    CHECK(fails("[journal]\nflush_interval_ms = soon\n"));
}

TEST_CASE(server_config, day_times_parse_and_format) {
    Timestamp ns = 0;
    CHECK(parse_day_time("00:00", ns));
    CHECK_EQ(ns, 0u);
    CHECK(parse_day_time("09:30:15", ns));
    CHECK_EQ(ns, (9 * 3600 + 30 * 60 + 15) * 1'000'000'000ull);
    CHECK(parse_day_time("23:59:59.999999999", ns));
    CHECK_EQ(ns, common::kNanosecondsPerDay - 1);
    CHECK(parse_day_time("24:00", ns));
    CHECK_EQ(ns, common::kNanosecondsPerDay);
    CHECK(!parse_day_time("24:00:01", ns));
    CHECK(!parse_day_time("12:60", ns));
    CHECK(!parse_day_time("12:00:60", ns));
    CHECK(!parse_day_time("12:00:00.1234567890", ns));
    CHECK(!parse_day_time("9:30", ns));
    CHECK(!parse_day_time("", ns));
    CHECK(format_day_time(0) == "00:00:00");
    CHECK(format_day_time(common::kNanosecondsPerDay) == "24:00:00");
    CHECK(format_day_time((9 * 3600 + 30 * 60) * 1'000'000'000ull + 250'000'000ull) == "09:30:00.25");
    CHECK(format_day_time(common::kNanosecondsPerDay - 1) == "23:59:59.999999999");
}

TEST_CASE(server_config, rejects_legacy_listeners_and_protocols) {
    ServerConfig cfg;
    std::string err;
    for (const auto key : {"fixp_sbe_port", "admin_port"}) {
        CHECK(!parse_config(std::string(kMinimal) + "\n[exchange]\n" + key + "=15003\n", cfg, err));
        CHECK(err.find("not supported") != std::string::npos);
    }
    for (const auto protocol : {"fixp_sbe", "sbe"})
        CHECK(!parse_config(std::string(kMinimal) + "\n[user LEGACY]\nsession_id=7\nprotocol=" + protocol + "\n", cfg,
                            err));
}

// A config with no user or no feed destination would start an exchange no one
// can trade on or watch. 0.0.0.0 stays legal for remote traders.
TEST_CASE(server_config, requires_a_user_and_a_feed_port) {
    ServerConfig cfg;
    std::string err;
    std::string text(kMinimal);
    CHECK(!parse_config(text.substr(0, text.find("[user ALICE]")), cfg, err));
    CHECK_EQ(err, "0: no [user] sections");
    CHECK(!parse_config(text + "[market_data]\nport = 0\n", cfg, err));
    CHECK_EQ(err, "0: market_data.port must be nonzero");
    CHECK(parse_config(text + "[exchange]\nbind_address = 0.0.0.0\n", cfg, err));
}

TEST_CASE(server_config, rejects_a_nul_byte_anywhere) {
    // "a.sock\0suffix" would open a.sock: the OS reads paths as C strings.
    std::string text = std::string(kMinimal) + "[exchange]\nadmin_socket = a.sock";
    text += '\0';
    text += "suffix\n";
    ServerConfig cfg;
    std::string err;
    CHECK(!parse_config(text, cfg, err));
    CHECK_EQ(err, "0: config contains a NUL byte");
}

// The publisher needs room for its largest ITCH message in one packet, so the
// parser must reject anything the publisher would refuse (QA review F3).
TEST_CASE(server_config, max_payload_matches_the_publisher_minimum) {
    auto parses = [](std::uint32_t max_payload) {
        ServerConfig cfg;
        std::string err;
        return parse_config(
            std::string(kMinimal) + "[market_data]\nmax_payload = " + std::to_string(max_payload) + "\n", cfg, err);
    };
    CHECK(!parses(64));
    CHECK(!parses(static_cast<std::uint32_t>(md::kMinimumMaxPayload) - 1));
    CHECK(parses(static_cast<std::uint32_t>(md::kMinimumMaxPayload)));
    CHECK(parses(static_cast<std::uint32_t>(md::kMaximumMaxPayload)));
    CHECK(!parses(static_cast<std::uint32_t>(md::kMaximumMaxPayload) + 1));

    ServerConfig cfg;
    std::string err;
    REQUIRE(parse_config(std::string(kMinimal) + "[market_data]\nmax_payload = 72\n", cfg, err));
    md::PublisherConfig publisher_config;
    publisher_config.session = cfg.market_data.session;
    publisher_config.max_payload = cfg.market_data.max_payload;
    CHECK(md::validate_publisher_config(publisher_config) == md::PublishStatus::Ok);
}
