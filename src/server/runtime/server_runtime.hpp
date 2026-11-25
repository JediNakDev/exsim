#ifndef EXSIM_SERVER_RUNTIME_SERVER_RUNTIME_HPP
#define EXSIM_SERVER_RUNTIME_SERVER_RUNTIME_HPP

// The exsim server: one thread, one reactor loop.
// BasicServerRuntime<Gateways...> owns the poller, the exchange core, the
// market-data runtime (ITCH or MDP, chosen from the config) and one
// order-entry endpoint per gateway:
//
//   poll_once():
//     poller.wait(busy_poll ? 0 : idle timeout)
//     route events: gateway connections (-> on_data -> core.submit ->
//                   dispatch to sessions and market data),
//                   market data recovery (MoldUDP64 requests or MDP replay),
//                   the owner's watched readable fd (the admin socket)
//     timer pass, at most once per idle_poll_timeout_ms:
//       core.timer(now): end of day, market data timers, journal flush
//       endpoints: delayed inbound bytes, gateway timers, requested closes
//     core disconnect requests (`fault user X disconnect`)
//     shutdown progress (end-of-day messages, drain, finish)
//     flush every connection with pending output (a queue: O(1) when none)
//
// Between timer passes an iteration does no timer work, which matters with
// busy_poll: the loop spins millions of times a second, while gateway
// deadlines (heartbeats, idle and logout timeouts) are seconds apart. A pass
// still walks every open connection; a per-connection deadline would need
// each gateway to report its next deadline, which is not worth it at
// max_connections slots per gateway and one pass per millisecond.
//
// Everything is allocated by the constructor and start(); poll_once does not
// allocate except on operator paths (admin commands, logging a connection
// failure). Gateways are compile-time template arguments: no virtual calls.

#include "common/clocks.hpp"
#include "net/poller.hpp"
#include "net/tcp_server.hpp"
#include "server/config.hpp"
#include "server/core/exchange_core.hpp"
#include "server/runtime/gateway_sessions.hpp"
#include "server/runtime/journal_worker.hpp"
#include "server/runtime/market_data_runtime.hpp"
#include "server/runtime/order_entry_endpoint.hpp"
#include "server/runtime/order_entry_gateways.hpp"
#include "server/runtime/server_clock.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace exsim::server {

struct ServerRuntimeOptions {
    bool busy_poll = false;  // poller.wait(0): lowest latency, one core at 100%
    // Timer resolution (delays and heartbeats): the longest idle wait, and
    // the interval between timer passes in both modes. 0 = every iteration.
    int idle_poll_timeout_ms = 1;
    // ns since local midnight at startup; nullopt reads the wall clock.
    std::optional<Timestamp> day_time_at_start;
    std::uint64_t end_of_day_drain_timeout_ns = 3'000'000'000;  // wait for logouts and flushes
    runtime::MarketDataRuntimeSettings market_data{};
    std::FILE* log = stderr;  // listeners at startup, feed send failures at shutdown; nullptr = silent
};

template <class... Gateways>
class BasicServerRuntime {
public:
    using Sessions = runtime::GatewaySessions<Gateways...>;
    using Core = ExchangeCore<Sessions, runtime::MarketDataRuntime, runtime::ServerClock, runtime::JournalWorker<>>;
    template <class Gateway>
    using Endpoint = runtime::OrderEntryEndpoint<Gateway, Core>;

    static constexpr std::uint8_t kRetransmissionPollTag = 0x41;
    static constexpr std::uint8_t kWatchedReadableFdPollTag = 0x42;
    // Called from poll_once with the context given to watch_readable_fd.
    using ReadableFdCallback = void (*)(void* context) noexcept;

    // Setup: builds everything. Throws std::runtime_error / invalid_argument
    // on a configuration the components refuse.
    BasicServerRuntime(const ServerConfig& config, ServerRuntimeOptions options)
        : config_(config),
          options_(std::move(options)),
          clock_(options_.day_time_at_start),
          market_data_(config_, options_.market_data),
          sessions_(config_),
          core_(config_, sessions_, market_data_, clock_),
          endpoints_(make_endpoint<Gateways>(gateway_index<Gateways>())...) {}

    BasicServerRuntime(const BasicServerRuntime&) = delete;
    BasicServerRuntime& operator=(const BasicServerRuntime&) = delete;

    // Opens every socket, then the journal, and sends the market-data start of
    // day. Returns "" or an error message.
    [[nodiscard]] std::string start() {
        if (net::ErrorCode error = poller_.open(); error != 0) return "poller: " + std::string(std::strerror(error));
        std::string failure;
        (open_endpoint<Gateways>(failure), ...);
        if (!failure.empty()) return failure;
        failure = market_data_.open(config_, poller_, net::make_poll_token(kRetransmissionPollTag, 0));
        if (!failure.empty()) return failure;
        try {
            core_.open_journal();  // last: a start refused above leaves the old journal alone
        } catch (const std::exception& error) {
            return config_.journal_path + ": " + error.what();
        }
        const auto now = core_.now();
        const md::PublishStatus started = market_data_.start(now, core_.engine(), core_.epoch_ns(now));
        if (started != md::PublishStatus::Ok) {
            return "market data start of day failed (status " + std::to_string(static_cast<int>(started)) + ")";
        }
        log_listeners();
        return {};
    }

    // One reactor iteration. Does nothing once finished(), so the shutdown
    // summary stays the final count of market data send failures.
    void poll_once() noexcept {
        if (finished_) return;
        const int timeout_ms = options_.busy_poll ? 0 : options_.idle_poll_timeout_ms;
        if (poller_.wait(timeout_ms) > 0) {
            for (const net::PollEvent& event : poller_.events()) route_event(event);
        }
        if (const std::uint64_t now = common::monotonic_ns(); now >= next_timer_pass_ns_) {
            next_timer_pass_ns_ = now + std::uint64_t(options_.idle_poll_timeout_ms) * 1'000'000;
            core_.timer(clock_.day_ns());
            (endpoint<Gateways>().run_timers(now), ...);
        }
        if (core_.has_disconnect_requests()) close_requested_sessions();
        if (!core_.shutdown_requested() &&
            (external_shutdown_requested_.load(std::memory_order_relaxed) || market_data_.stopped())) {
            core_.shutdown();
        }
        if (core_.shutdown_requested()) progress_shutdown();
        (endpoint<Gateways>().flush(), ...);
        market_data_.flush();
    }

    void run() noexcept {
        while (!finished()) poll_once();
    }

    // After start(): registers one owner fd (the admin socket or its client) with
    // the poller, so a readable fd wakes an idle wait and poll_once calls
    // on_readable(context) when it is readable or hung up. The callback runs on
    // the server thread and may call request_shutdown or unwatch_readable_fd.
    // Replaces a previously watched fd. Returns 0 or an errno value.
    [[nodiscard]] net::ErrorCode watch_readable_fd(int fd, ReadableFdCallback on_readable, void* context) noexcept {
        unwatch_readable_fd();
        if (net::ErrorCode error =
                poller_.add(fd, net::make_poll_token(kWatchedReadableFdPollTag, static_cast<std::uint64_t>(fd)),
                            net::PollInterest::Read);
            error != 0) {
            return error;
        }
        watched_readable_fd_ = fd;
        on_watched_fd_readable_ = on_readable;
        watched_fd_context_ = context;
        return 0;
    }
    // Unregisters the watched fd; call it before closing that fd. Later events
    // already returned by the current wait are ignored.
    void unwatch_readable_fd() noexcept {
        if (watched_readable_fd_ < 0) return;
        (void)poller_.remove(watched_readable_fd_);
        watched_readable_fd_ = -1;
        on_watched_fd_readable_ = nullptr;
        watched_fd_context_ = nullptr;
    }

    // Any thread (and signal handlers): ends the trading day at the next
    // iteration, as the admin `shutdown` command does.
    void request_shutdown() noexcept { external_shutdown_requested_.store(true, std::memory_order_relaxed); }
    // The day has ended and every session has been closed (or the drain
    // timed out): the process may exit.
    [[nodiscard]] bool finished() const noexcept { return finished_; }

    // ---- access (server thread, or after it stopped) ---------------------------
    [[nodiscard]] Core& core() noexcept { return core_; }
    [[nodiscard]] const Core& core() const noexcept { return core_; }
    [[nodiscard]] const ServerConfig& config() const noexcept { return config_; }
    template <class Gateway>
    [[nodiscard]] Endpoint<Gateway>& endpoint() noexcept {
        return *std::get<std::unique_ptr<Endpoint<Gateway>>>(endpoints_);
    }
    template <class Gateway>
    [[nodiscard]] net::Ipv4Endpoint listen_endpoint() noexcept {
        return endpoint<Gateway>().listen_endpoint();
    }
    [[nodiscard]] net::Ipv4Endpoint retransmission_endpoint() const noexcept {
        return market_data_.retransmission_endpoint();
    }

private:
    template <class Gateway>
    static constexpr std::uint8_t gateway_index() noexcept {
        std::uint8_t index = 0;
        std::uint8_t found = 0;
        ((std::is_same_v<Gateway, Gateways> ? (found = index, ++index) : ++index), ...);
        return found;
    }
    template <class Gateway>
    static constexpr std::uint8_t gateway_poll_tag() noexcept {
        return static_cast<std::uint8_t>(gateway_index<Gateway>() + 1);
    }

    template <class Gateway>
    std::unique_ptr<Endpoint<Gateway>> make_endpoint(std::uint8_t index) {
        net::TcpServerConfig tcp;
        tcp.listen_endpoint = net::Ipv4Endpoint{parse_address(config_.exchange.bind_address, "exchange.bind_address"),
                                                Gateway::configured_port(config_)};
        tcp.max_connections = config_.exchange.max_connections;
        // Holds the largest single write: an iLink retransmission of 2500
        // reports of up to about 500 bytes. Untouched pages are not resident.
        tcp.send_buffer_capacity = 4 * 1024 * 1024;
        if constexpr (requires { Gateway::enabled(config_); }) {
            if (!Gateway::enabled(config_)) tcp.max_connections = 0;
        }
        return std::make_unique<Endpoint<Gateway>>(tcp, poller_, static_cast<std::uint8_t>(index + 1),
                                                   sessions_.template user_directory<Gateway>(), core_);
    }

    static std::uint32_t parse_address(const std::string& text, const char* what) {
        std::uint32_t address = 0;
        if (!net::parse_ipv4_address(text, address)) {
            throw std::invalid_argument(std::string(what) + " is not an IPv4 address: " + text);
        }
        return address;
    }

    template <class Gateway>
    void open_endpoint(std::string& failure) {
        if (!failure.empty()) return;
        if constexpr (requires { Gateway::enabled(config_); })
            if (!Gateway::enabled(config_)) return;
        if (net::ErrorCode error = endpoint<Gateway>().open(); error != 0) {
            failure = std::string(Gateway::name) + " listener on " + config_.exchange.bind_address + ":" +
                      std::to_string(Gateway::configured_port(config_)) + ": " + std::strerror(error);
        }
    }

    void route_event(const net::PollEvent& event) noexcept {
        const std::uint8_t tag = net::poll_token_tag(event.token);
        if (tag == kRetransmissionPollTag) {
            market_data_.handle_recovery_event(event);
            return;
        }
        if (tag == kWatchedReadableFdPollTag) {
            if (watched_readable_fd_ >= 0 &&
                net::poll_token_value(event.token) == static_cast<std::uint64_t>(watched_readable_fd_) &&
                (event.readable || event.hangup || event.error)) {
                on_watched_fd_readable_(watched_fd_context_);
            }
            return;
        }
        (void)((tag == gateway_poll_tag<Gateways>() ? (endpoint<Gateways>().handle_event(event), true) : false) || ...);
    }

    // `fault user X disconnect`: the connection is closed without flushing
    // and the gateway's on_closed unbinds the session (cancel-on-disconnect).
    void close_requested_sessions() noexcept {
        std::array<SessionId, 64> sessions{};
        for (;;) {
            const std::size_t count = core_.take_disconnect_requests(sessions);
            for (std::size_t index = 0; index < count; ++index) {
                const std::optional<ConnectionId> connection = core_.connection(sessions[index]);
                if (!connection) continue;
                (void)((runtime::core_connection_endpoint_tag(*connection) == gateway_poll_tag<Gateways>() &&
                        endpoint<Gateways>().close_core_connection(*connection)) ||
                       ...);
            }
            if (count < sessions.size()) return;
        }
    }

    // End of day: OUCH End of Session, FIX Logout and FIXP Terminate on every
    // connection, then wait (bounded) for the sessions to close and flush.
    void progress_shutdown() noexcept {
        if (finished_) return;
        const std::uint64_t now = common::monotonic_ns();
        if (!end_of_day_started_) {
            end_of_day_started_ = true;
            drain_deadline_ns_ = now + options_.end_of_day_drain_timeout_ns;
            (endpoint<Gateways>().close_for_end_of_day(), ...);
        }
        std::uint32_t remaining = market_data_.recovery_connection_count();
        ((remaining += endpoint<Gateways>().transport_connection_count()), ...);
        if (remaining != 0 && now < drain_deadline_ns_) return;
        if (remaining != 0) {
            (endpoint<Gateways>().close_all_now(), ...);
            market_data_.close_recovery_now();
        }
        if (!journal_drain_started_) {
            journal_drain_started_ = true;
            (void)core_.flush_journal();
        }
        if (!core_.journal_drained()) {
            if (now < drain_deadline_ns_) return;
            core_.abort_journal();
        }
        // status shows the count while running; this covers runs without an
        // admin socket. Printed last: ITCH repeats End of Session during the drain.
        market_data_.flush();
        if (const std::uint64_t failures = market_data_.send_failure_count();
            failures != 0 && options_.log != nullptr) {
            std::fprintf(options_.log, "exsim: market data UDP send failures: %llu\n",
                         static_cast<unsigned long long>(failures));
            std::fflush(options_.log);
        }
        finished_ = true;
    }

    void log_listeners() {
        if (options_.log == nullptr) return;
        std::array<char, net::kIpv4EndpointTextCapacity> text{};
        const auto print = [&](std::string_view what, net::Ipv4Endpoint where) {
            if (where.port == 0) return;
            const std::size_t length = net::format_ipv4_endpoint(where, text);
            std::fprintf(options_.log, "exsim: %.*s listening on %.*s\n", static_cast<int>(what.size()), what.data(),
                         static_cast<int>(length), text.data());
        };
        (print(Gateways::name, endpoint<Gateways>().listen_endpoint()), ...);
        print("md retransmission", market_data_.retransmission_endpoint());
        const std::size_t length = net::format_ipv4_endpoint(market_data_.feed_destination(), text);
        std::fprintf(options_.log, "exsim: md feed to %.*s%s\n", static_cast<int>(length), text.data(),
                     market_data_.feed_destination().port == 0 ? " (port 0: feed disabled)" : "");
        std::fflush(options_.log);
    }

    ServerConfig config_;
    ServerRuntimeOptions options_;
    net::Poller poller_;
    runtime::ServerClock clock_;
    runtime::MarketDataRuntime market_data_;
    Sessions sessions_;
    Core core_;
    std::tuple<std::unique_ptr<Endpoint<Gateways>>...> endpoints_;
    std::atomic<bool> external_shutdown_requested_{false};
    int watched_readable_fd_ = -1;
    ReadableFdCallback on_watched_fd_readable_ = nullptr;
    void* watched_fd_context_ = nullptr;
    bool end_of_day_started_ = false;
    bool journal_drain_started_ = false;
    bool finished_ = false;
    std::uint64_t drain_deadline_ns_ = 0;
    std::uint64_t next_timer_pass_ns_ = 0;
};

// The active exchange server: OUCH 4.2 over SoupBinTCP, FIX 4.4 and iLink 3.
using ServerRuntime = BasicServerRuntime<runtime::OuchGateway, runtime::FixGateway, runtime::IlinkGateway>;

}  // namespace exsim::server

#endif
