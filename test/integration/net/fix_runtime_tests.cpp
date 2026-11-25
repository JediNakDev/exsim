#include "server/admin/core_admin.hpp"
#include "server/runtime/server_runtime.hpp"
#include "test/exsim/gateway/fix/fix_test_support.hpp"
#include "test/framework/test.hpp"

#include <array>
#include <cstdio>
#include <memory>
#include <netinet/in.h>

namespace {
using Runtime = exsim::server::ServerRuntime;

exsim::net::Socket connect_fix(Runtime& runtime) {
    exsim::net::Socket socket(::socket(AF_INET, SOCK_STREAM, 0));
    REQUIRE(socket.is_open());
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(runtime.listen_endpoint<exsim::server::runtime::FixGateway>().port);
    REQUIRE_EQ(::connect(socket.fd(), reinterpret_cast<sockaddr*>(&address), sizeof address), 0);
    REQUIRE_EQ(exsim::net::set_non_blocking(socket.fd()), 0);
    REQUIRE_EQ(exsim::net::set_no_sigpipe(socket.fd()), 0);
    return socket;
}

// Sends `frame` and polls until a whole FIX message arrives. The deadline is set
// after the first pass (which may only accept the connection) and restarts
// whenever bytes arrive. Only a pass that starts after it and receives nothing
// fails the test, so a descheduled thread does not.
std::string exchange_frame(Runtime& runtime, int fd, std::string_view frame) {
    REQUIRE_EQ(exsim::net::send_some(fd, frame.data(), frame.size()), static_cast<ssize_t>(frame.size()));
    std::string output;
    std::array<char, 8192> buffer{};
    for (std::uint64_t deadline = 0;;) {
        const auto pass_started = exsim::common::monotonic_ns();
        runtime.poll_once();
        const auto size = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (size > 0)
            output.append(buffer.data(), static_cast<std::size_t>(size));
        else if (size < 0)
            REQUIRE(exsim::net::is_would_block(errno));
        if (output.find("\00110=") != std::string::npos && output.back() == '\001') return output;
        if (deadline == 0 || size > 0)
            deadline = exsim::common::monotonic_ns() + 2'000'000'000;
        else if (pass_started >= deadline)
            FAIL("no whole FIX message before the deadline");
    }
}

// Every configured user speaks FIX; all ports are ephemeral or off.
exsim::server::ServerConfig fix_config() {
    exsim::server::ServerConfig config;
    std::string error;
    REQUIRE(exsim::server::load_config("config/exchange.conf", config, error));
    config.exchange.ouch_port = 0;
    config.exchange.fix_port = 0;
    config.market_data.port = 0;
    config.market_data.retransmit_port = 0;
    for (auto& user : config.users) user.protocol = exsim::server::Protocol::Fix44;
    return config;
}

exsim::server::ServerRuntimeOptions fix_options() {
    exsim::server::ServerRuntimeOptions options;
    options.log = nullptr;
    options.day_time_at_start = fix_test::kSessionStartNs;
    return options;
}
}  // namespace

TEST_CASE(fix_runtime, tcp_logon_orders_matching_and_logout) {
    Runtime runtime(fix_config(), fix_options());
    REQUIRE(runtime.start().empty());
    CHECK_NE(runtime.listen_endpoint<exsim::server::runtime::FixGateway>().port,
             runtime.listen_endpoint<exsim::server::runtime::OuchGateway>().port);
    auto alice = connect_fix(runtime);
    auto bob = connect_fix(runtime);
    auto send = [&](int fd, std::string_view user, std::string_view type, unsigned seq, std::string_view fields) {
        return fix_test::with_pipes(exchange_frame(runtime, fd, fix_test::client_message(type, seq, fields, user)));
    };
    CHECK(send(alice.fd(), "ALICE", "A", 1, "98=0|108=30|554=alice|").find("35=A|") != std::string::npos);
    CHECK(send(bob.fd(), "BOB", "A", 1, "98=0|108=30|554=bob|").find("35=A|") != std::string::npos);
    CHECK(send(alice.fd(), "ALICE", "D", 2, fix_test::new_order_fields("BUY", "1", 100, "150")).find("39=0|") !=
          std::string::npos);
    CHECK_EQ(runtime.core().engine().live_order_count(), 1u);
    CHECK(send(bob.fd(), "BOB", "D", 2, fix_test::new_order_fields("SELL", "2", 100, "150")).find("39=2|") !=
          std::string::npos);
    CHECK_EQ(runtime.core().engine().live_order_count(), 0u);
    CHECK(send(bob.fd(), "BOB", "5", 3, "").find("35=5|") != std::string::npos);
    // Peer EOF unbinds both sessions.
    alice.close();
    bob.close();
    const auto deadline = exsim::common::monotonic_ns() + 2'000'000'000;
    while (runtime.core().bound_connection_count() != 0 && exsim::common::monotonic_ns() < deadline)
        runtime.poll_once();
    CHECK_EQ(runtime.core().bound_connection_count(), 0u);
}

// A FIX session that never answers the end-of-day Logout holds the drain open
// while ITCH repeats End of Session every heartbeat to a broadcast address
// (no SO_BROADCAST, so every send fails). The shutdown summary must count
// those sends too, and nothing may send after it.
TEST_CASE(fix_runtime, shutdown_summary_counts_feed_send_failures_during_the_drain) {
    exsim::server::ServerConfig config;
    std::string error;
    REQUIRE(exsim::server::load_config("config/exchange.conf", config, error));
    config.exchange.ouch_port = 0;
    config.exchange.fix_port = 0;
    config.market_data.address = "255.255.255.255";
    config.market_data.retransmit_port = 0;
    for (auto& user : config.users) user.protocol = exsim::server::Protocol::Fix44;
    const std::unique_ptr<std::FILE, int (*)(std::FILE*)> log_file(std::tmpfile(), &std::fclose);
    REQUIRE(log_file != nullptr);
    exsim::server::ServerRuntimeOptions options;
    options.log = log_file.get();
    options.day_time_at_start = fix_test::kSessionStartNs;
    options.market_data.heartbeat_interval_ns = 1'000'000;
    options.end_of_day_drain_timeout_ns = 100'000'000;
    Runtime runtime(config, options);
    REQUIRE(runtime.start().empty());
    auto alice = connect_fix(runtime);
    const auto logon =
        exchange_frame(runtime, alice.fd(), fix_test::client_message("A", 1, "98=0|108=30|554=alice|", "ALICE"));
    REQUIRE(fix_test::with_pipes(logon).find("35=A|") != std::string::npos);
    runtime.request_shutdown();
    const auto deadline = exsim::common::monotonic_ns() + 2'000'000'000;
    while (!runtime.finished() && exsim::common::monotonic_ns() < deadline) runtime.poll_once();
    REQUIRE(runtime.finished());
    // Polling on after finished() spans several heartbeat intervals.
    const auto finished_at = exsim::common::monotonic_ns();
    while (exsim::common::monotonic_ns() - finished_at < 10'000'000) runtime.poll_once();
    std::string reply;
    exsim::server::CoreAdmin(runtime.core()).execute_line("status", reply);
    const auto at = reply.find(" send_failures=");
    REQUIRE(at != std::string::npos);
    const auto failures = std::stoull(reply.substr(at + std::string_view(" send_failures=").size()));
    std::rewind(log_file.get());
    std::string log(4096, '\0');
    log.resize(std::fread(log.data(), 1, log.size(), log_file.get()));
    constexpr std::string_view kSummary = "exsim: market data UDP send failures: ";
    const auto line = log.find(kSummary);
    REQUIRE(line != std::string::npos);
    CHECK_GT(failures, 0u);
    CHECK_EQ(std::stoull(log.substr(line + kSummary.size())), failures);
}

// A test that fails a REQUIRE leaves with sessions still bound. Destroying the
// runtime then ends them; it used to abort in the FixConnection destructor.
TEST_CASE(fix_runtime, destroying_the_runtime_ends_bound_sessions) {
    Runtime runtime(fix_config(), fix_options());
    REQUIRE(runtime.start().empty());
    auto alice = connect_fix(runtime);
    const auto logon = fix_test::client_message("A", 1, "98=0|108=30|554=alice|", "ALICE");
    CHECK(fix_test::with_pipes(exchange_frame(runtime, alice.fd(), logon)).find("35=A|") != std::string::npos);
    CHECK_EQ(runtime.core().bound_connection_count(), 1u);
}
