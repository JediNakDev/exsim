// poll_once runs gateway timers in a timer pass at most once per
// idle_poll_timeout_ms, not on every iteration of a busy-polling loop.

#include "common/clocks.hpp"
#include "server/runtime/server_runtime.hpp"
#include "test/framework/test.hpp"

#include <algorithm>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
using namespace exsim;

std::uint64_t gateway_timer_calls = 0;

// The OUCH gateway with a count of Connection::on_timer calls.
struct CountingOuchGateway : server::runtime::OuchGateway {
    template <class Core>
    class Connection : public server::runtime::OuchGateway::Connection<Core> {
    public:
        using Base = server::runtime::OuchGateway::Connection<Core>;
        using Base::Base;
        void on_timer(Core& core) noexcept {
            ++gateway_timer_calls;
            Base::on_timer(core);
        }
    };
};
using CountingRuntime = server::BasicServerRuntime<CountingOuchGateway>;

server::ServerConfig ouch_config() {
    server::ServerConfig config;
    std::string error;
    REQUIRE(server::load_config("config/examples/exchange.conf", config, error));
    config.exchange.ouch_port = 0;
    config.market_data.port = 0;
    config.market_data.retransmit_port = 0;
    return config;
}

// Busy-polling runtime with one accepted (not logged in) OUCH connection.
struct ConnectedRuntime {
    CountingRuntime runtime;
    int client = -1;

    explicit ConnectedRuntime(int timer_interval_ms) : runtime(ouch_config(), options(timer_interval_ms)) {
        REQUIRE(runtime.start().empty());
        client = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(client >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(runtime.listen_endpoint<CountingOuchGateway>().port);
        REQUIRE_EQ(::connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof address), 0);
        const std::uint64_t deadline = common::monotonic_ns() + 2'000'000'000;
        auto& endpoint = runtime.endpoint<CountingOuchGateway>();
        while (endpoint.transport_connection_count() == 0 && common::monotonic_ns() < deadline) runtime.poll_once();
        REQUIRE_EQ(endpoint.transport_connection_count(), 1u);
    }
    ~ConnectedRuntime() { ::close(client); }

    static server::ServerRuntimeOptions options(int timer_interval_ms) {
        server::ServerRuntimeOptions options;
        options.log = nullptr;
        options.busy_poll = true;
        options.idle_poll_timeout_ms = timer_interval_ms;
        return options;
    }
};

}  // namespace

TEST_CASE(server_runtime_timers, busy_poll_iterations_skip_timers_until_the_next_pass_is_due) {
    ConnectedRuntime connected(5'000);
    const std::uint64_t calls_before = gateway_timer_calls;
    const std::uint64_t started = common::monotonic_ns();
    for (int iteration = 0; iteration < 2'000; ++iteration) connected.runtime.poll_once();
    REQUIRE(common::monotonic_ns() - started < 5'000'000'000);  // the next pass is not due yet
    CHECK_EQ(gateway_timer_calls - calls_before, 0u);
}

TEST_CASE(server_runtime_timers, timer_passes_run_once_per_interval) {
    ConnectedRuntime connected(1);
    const std::uint64_t calls_before = gateway_timer_calls;
    const std::uint64_t started = common::monotonic_ns();
    std::uint64_t calls = 0, late_iterations = 0, last_call_ns = 0, max_poll_calls = 0;
    // At least 50 ms and five passes: a loaded machine can deschedule the loop
    // for most of any 50 ms window, and passes need the loop to run.
    while ((calls < 5 || common::monotonic_ns() - started < 50'000'000) &&
           common::monotonic_ns() - started < 5'000'000'000) {
        const std::uint64_t iteration_started = common::monotonic_ns();
        connected.runtime.poll_once();
        const std::uint64_t now_calls = gateway_timer_calls - calls_before;
        // last_call_ns is read after the pass, so an iteration starting 1 ms
        // later is due by the runtime's clock too, however long it was descheduled.
        if (now_calls > calls)
            last_call_ns = common::monotonic_ns();
        else if (last_call_ns != 0 && iteration_started - last_call_ns >= 1'000'000)
            ++late_iterations;
        max_poll_calls = std::max(max_poll_calls, now_calls - calls);
        calls = now_calls;
    }
    const std::uint64_t elapsed_ms = (common::monotonic_ns() - started) / 1'000'000;
    CHECK_GE(calls, 5u);              // the connection's timers still run
    CHECK_LE(calls, elapsed_ms + 1);  // but at most once per millisecond
    CHECK_EQ(late_iterations, 0u);    // and on the first iteration that is due
    CHECK_EQ(max_poll_calls, 1u);     // and one call per pass for the one connection
}
