// Wire benchmark: each order's OUCH ack and ITCH feed message from a running exchange, timed at the client.
//
// Starts exsim_exchanged by the path it was run by with the last name changed
// (a symlink to this binary is not followed; the report names the path), with a
// generated config written beside it as wire_latency.conf, logs in one OUCH 4.2
// user over loopback TCP and receives the ITCH 5.0 feed as MoldUDP64 unicast to
// 127.0.0.1. Each order is timed from before the client encodes its Enter Order
// to after the client decodes
//   ack:  its OUCH Order Accepted, and
//   feed: its ITCH Add Order (rest), or the ITCH Order Executed for the
//         resting order it fills (cross).
// benchmark/README.md states what the numbers include and exclude.
//
// Each workload runs at window 1 (one order in flight) and at --window:
//   rest:  N buys at $100.00 that rest.
//   cross: N sells at $150.00 rest (untimed), then N timed buys at $150.00
//          each fill one of them fully.
// Acks and feed messages must arrive in the order the orders were sent. Each
// ack must name its order's token; each feed message must name the order
// reference of its ack (rest) or of the resting order's ack (cross). A
// mismatch, a partial fill or a lost packet fails the run.

#include "gateway/ouch/ouch_mapping.hpp"
#include "net/socket.hpp"
#include "net/udp.hpp"
#include "protocol/itch50.hpp"
#include "protocol/moldudp64.hpp"
#include "protocol/ouch42.hpp"
#include "protocol/soupbintcp.hpp"
#include "utils/cpu_pinner.hpp"
#include "utils/csv_writer.hpp"
#include "utils/histogram.hpp"
#include "utils/stats.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

extern char** environ;

namespace {

using namespace exsim;
namespace soup = protocol::soupbintcp;
namespace ouch42 = protocol::ouch42;
namespace itch = protocol::itch50;

constexpr Price kRestPrice = 1'000'000;   // $100.00 (four implied decimals on the OUCH wire)
constexpr Price kCrossPrice = 1'500'000;  // $150.00
constexpr Quantity kShares = 100;
constexpr std::uint64_t kStallNs = 10'000'000'000;  // no progress for this long fails the run

std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error(what); }

// The signals that end a run. The handler stores the number, so the run ends through the destructors
// that stop the exchange instead of leaving it running.
constexpr int kStopSignals[] = {SIGHUP, SIGINT, SIGPIPE, SIGTERM};
volatile sig_atomic_t interrupted = 0;

// Called once the exchange is gone: a signal that came, or comes from now on, ends this process by its
// default action, so no message waits for a reader that stops reading, and the parent sees which signal it was.
void end_on_signals() {
    for (const int number : kStopSignals) ::signal(number, SIG_DFL);
    if (interrupted != 0) ::raise(interrupted);
}

// Every wait calls this: a signal, or kStallNs since `progress_ns`, ends the run.
void check(std::uint64_t progress_ns, const char* waiting_for) {
    if (interrupted != 0) fail("interrupted");
    if (now_ns() - progress_ns > kStallNs) fail(std::string("no ") + waiting_for + " within 10 s");
}

template <class Number>
bool parse_number(std::string_view text, Number& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

// Capacity for both windows, `orders` per pass: 6N tokens, at most 3N resting
// orders, and 5N OUCH messages per window (an Accepted per order plus two
// Executed per cross) with room for the gateway's store reserve.
std::string config_text(std::uint16_t feed_port, std::size_t orders) {
    const std::size_t messages = 10 * orders + 2048;
    return "[exchange]\n"
           "name = EXSIM\n"
           "bind_address = 127.0.0.1\n"
           "ouch_port = 0\n"
           "max_connections = 1\n"
           "[market_data]\n"
           "address = 127.0.0.1\n"
           "port = " +
           std::to_string(feed_port) +
           "\n"
           "retransmit_port = 0\n"
           "[engine]\n"
           "max_instruments = 1\n"
           "max_orders = " +
           std::to_string(3 * orders + 16) +
           "\n"
           "max_tokens_per_session = " +
           std::to_string(6 * orders + 16) +
           "\n"
           "store_messages_per_session = " +
           std::to_string(messages) +
           "\n"
           "store_bytes_per_session = " +
           std::to_string(128 * messages) +
           "\n"
           "[instrument BENCH]\n"
           "tick_size = 0.01\n"
           "lot_size = 1\n"
           "min_price = 0.01\n"
           "max_price = 1000.00\n"
           "reference_price = 150.00\n"
           "initial_phase = continuous\n"
           "[user BENCH]\n"
           "password = bench\n"
           "protocol = ouch42\n"
           "session_id = 1\n"
           "owner = 0\n";
}

// The exchange under test: a child process. stop() ends it with SIGTERM, as an
// operator would; the destructor kills it if the run failed first.
class ExchangeProcess {
public:
    ExchangeProcess(std::string binary, std::string config) {
        int out[2];
        if (::pipe(out) != 0) fail(std::string("pipe: ") + std::strerror(errno));
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, out[0]);
        posix_spawn_file_actions_addclose(&actions, out[1]);
        std::string flag = "--config";
        char* argv[] = {binary.data(), flag.data(), config.data(), nullptr};
        const int error = posix_spawn(&pid_, binary.c_str(), &actions, nullptr, argv, environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(out[1]);
        if (error != 0) {
            ::close(out[0]);
            fail(binary + ": " + std::strerror(error));
        }
        ready_fd_ = out[0];
    }
    ~ExchangeProcess() {
        if (ready_fd_ >= 0) ::close(ready_fd_);
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
        }
    }
    ExchangeProcess(const ExchangeProcess&) = delete;
    ExchangeProcess& operator=(const ExchangeProcess&) = delete;

    // The one JSON line the exchange prints on stdout once it is listening.
    std::string read_ready_line() {
        std::string line;
        char byte = 0;
        const std::uint64_t start = now_ns();
        while (byte != '\n') {
            check(start, "ready line from the exchange");
            pollfd ready{ready_fd_, POLLIN, 0};
            if (::poll(&ready, 1, 100) != 1) continue;
            const ssize_t read = ::read(ready_fd_, &byte, 1);
            if (read == 0) fail("the exchange exited before its ready line");
            if (read < 0 && errno != EINTR) fail(std::string("read: ") + std::strerror(errno));
            if (read == 1) line += byte;
        }
        ::close(ready_fd_);
        ready_fd_ = -1;
        return line;
    }

    // SIGTERM, then the exit code, or -1 if it did not exit normally. If it
    // does not exit in time, the destructor kills it.
    int stop() {
        ::kill(pid_, SIGTERM);
        const std::uint64_t start = now_ns();
        int status = 0;
        pid_t done = 0;
        while ((done = ::waitpid(pid_, &status, WNOHANG)) == 0 || (done < 0 && errno == EINTR)) {
            check(start, "exchange exit after SIGTERM");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (done < 0) fail(std::string("waitpid: ") + std::strerror(errno));
        pid_ = -1;
        // No deadline once reaped: the client may have been descheduled past it. end_on_signals() in main()
        // acts on a signal from here on.
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

private:
    pid_t pid_ = -1;
    int ready_fd_ = -1;
};

// The value of `key` in the ready line, a flat JSON object of numbers and
// quoted text with no escapes.
std::string_view ready_field(std::string_view line, std::string_view key) {
    const std::string quoted = "\"" + std::string(key) + "\":";
    const std::size_t at = line.find(quoted);
    if (at == std::string_view::npos) fail("no " + std::string(key) + " in the ready line: " + std::string(line));
    line.remove_prefix(at + quoted.size());
    if (line.starts_with('"')) return line.substr(1, line.find('"', 1) - 1);
    return line.substr(0, line.find_first_of(",}"));
}

// One workload pass. Latencies are in nanoseconds; references are the
// exchange's order references, indexed by order.
struct Pass {
    bench::LatencyHistogram ack;
    bench::LatencyHistogram feed;
    std::vector<std::uint64_t> ack_references;
    std::vector<std::uint64_t> feed_references;
    double orders_per_second = 0;
};

// One OUCH session and the feed. Both sockets are non-blocking and the client
// spins on them, so its own wake-up is not part of any number.
class WireClient {
public:
    WireClient() {
        net::Ipv4Endpoint local{};
        net::ErrorCode error = feed_.open(net::loopback_endpoint(0));
        if (error == 0) error = net::local_endpoint(feed_.fd(), local);
        if (error != 0) fail(std::string("feed socket: ") + std::strerror(error));
        feed_port_ = local.port;
    }

    [[nodiscard]] std::uint16_t feed_port() const noexcept { return feed_port_; }

    // `feed_session` is the exchange's MoldUDP64 session, from its ready line.
    void log_in(std::uint16_t port, std::string_view feed_session) {
        if (feed_session.size() != feed_session_.size())
            fail("bad MoldUDP64 session in the ready line: " + std::string(feed_session));
        std::ranges::copy(feed_session, feed_session_.begin());
        net::ErrorCode error = net::open_tcp_socket(order_entry_);
        if (error == 0) error = net::set_tcp_no_delay(order_entry_.fd(), true);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(net::kIpv4LoopbackAddress);
        address.sin_port = htons(port);
        if (error == 0 && ::connect(order_entry_.fd(), reinterpret_cast<const sockaddr*>(&address), sizeof address) < 0)
            error = errno;
        if (error == EINPROGRESS) {
            const std::uint64_t start = now_ns();
            pollfd connected{order_entry_.fd(), POLLOUT, 0};
            while (::poll(&connected, 1, 100) != 1) check(start, "connection to the OUCH port");
            socklen_t size = sizeof error;
            if (::getsockopt(order_entry_.fd(), SOL_SOCKET, SO_ERROR, &error, &size) < 0) error = errno;
        }
        if (error != 0) fail(std::string("connect to the OUCH port: ") + std::strerror(error));

        soup::LoginRequest request;
        protocol::wire::alpha(request.username.data(), request.username.size(), "BENCH");
        protocol::wire::alpha(request.password.data(), request.password.size(), "bench");
        request.sequence = 1;
        char frame[64];
        send_all(frame, soup::encode(request, frame, sizeof frame));
        bool accepted = false;
        const std::uint64_t start = now_ns();
        while (!accepted) {
            read_order_entry([&](const soup::Packet& packet) {
                if (packet.type == 'J') fail("login rejected");
                accepted = accepted || packet.type == 'A';
            });
            read_feed([](std::span<const char>) {});
            check(start, "login reply");
        }
    }

    void close() noexcept { order_entry_.close(); }

    // Sends `count` orders on one side at one price with at most `window` in
    // flight, and returns once each has its Order Accepted and its feed
    // message of `feed_type`.
    Pass run(char side, Price price, std::size_t count, std::size_t window, char feed_type) {
        Pass pass;
        pass.ack_references.assign(count, 0);
        pass.feed_references.assign(count, 0);
        std::vector<std::uint64_t> sent_ns(count, 0);
        const std::uint64_t first_token = next_token_;
        next_token_ += count;
        std::size_t sent = 0;
        std::size_t acked = 0;
        std::size_t fed = 0;

        const auto on_packet = [&](const soup::Packet& packet) {
            if (packet.type != 'S' || packet.payload.empty()) return;  // heartbeats
            if (packet.payload.front() == ouch42::OrderRejected::type) {
                ouch42::OrderRejected rejected;
                (void)ouch42::decode(packet.payload.data(), packet.payload.size(), rejected);
                fail(std::string("order rejected, reason ") + rejected.reason);
            }
            if (packet.payload.front() != ouch42::OrderAccepted::type) return;  // executions, system events
            ouch42::OrderAccepted accepted;
            if (ouch42::decode(packet.payload.data(), packet.payload.size(), accepted) != protocol::Status::ok)
                fail("undecodable Order Accepted");
            const std::string_view token = protocol::wire::alpha(accepted.token.data(), accepted.token.size());
            std::uint64_t number = 0;
            // The client's tokens have no leading zero, so "01" is not token 1.
            if (acked == sent || token.starts_with('0') || !parse_number(token, number) ||
                number != first_token + acked)
                fail("Order Accepted out of order: token " + std::string(token));
            pass.ack.record(now_ns() - sent_ns[acked]);
            pass.ack_references[acked++] = accepted.order_reference;
        };
        const auto on_message = [&](std::span<const char> message) {
            if (message.front() != feed_type) return;
            if (fed == sent) fail(std::string("feed message '") + feed_type + "' with no order outstanding");
            std::uint64_t reference = 0;
            protocol::Status status;
            if (feed_type == itch::AddOrder::type) {
                itch::AddOrder add;
                status = itch::decode(message, add);
                reference = add.order_reference;
            } else {
                itch::OrderExecuted executed;
                status = itch::decode(message, executed);
                reference = executed.order_reference;
                if (status == protocol::Status::ok && executed.shares != kShares)
                    fail("Order Executed for " + std::to_string(executed.shares) + " shares, not a full fill");
            }
            if (status != protocol::Status::ok) fail(std::string("undecodable feed message '") + feed_type + "'");
            pass.feed.record(now_ns() - sent_ns[fed]);
            pass.feed_references[fed++] = reference;
        };

        const std::uint64_t start = now_ns();
        std::uint64_t progress_ns = start;
        std::size_t progress = 0;
        while (acked < count || fed < count) {
            if (sent < count && sent - std::min(acked, fed) < window) {
                sent_ns[sent] = now_ns();
                send_order(first_token + sent, side, price);
                ++sent;
            }
            read_order_entry(on_packet);
            read_feed(on_message);
            const std::uint64_t now = now_ns();
            if (sent + acked + fed != progress) {
                progress = sent + acked + fed;
                progress_ns = now;
            }
            if (interrupted != 0 || now - progress_ns > kStallNs) {  // the message only when failing
                const std::string what = "ack or feed message (sent " + std::to_string(sent) + ", acked " +
                                         std::to_string(acked) + ", on the feed " + std::to_string(fed) + " of " +
                                         std::to_string(count) + ")";
                check(progress_ns, what.c_str());
            }
        }
        pass.orders_per_second = static_cast<double>(count) * 1e9 / static_cast<double>(now_ns() - start);
        return pass;
    }

private:
    void send_order(std::uint64_t token, char side, Price price) {
        ouch42::EnterOrder order;
        std::to_chars(order.token.data(), order.token.data() + order.token.size(), token);
        order.side = side;
        order.shares = kShares;
        order.stock = Symbol("BENCH");
        order.price = price;
        order.time_in_force = gateway::ouch::kOuchTimeInForceSystemHours;
        protocol::wire::alpha(order.firm.data(), order.firm.size(), "BNCH");
        order.display = 'Y';
        order.capacity = 'A';
        order.ise = 'N';
        order.cross_type = 'N';
        order.customer_type = ' ';
        char payload[ouch42::EnterOrder::wire_size];
        char frame[ouch42::EnterOrder::wire_size + 3];
        ouch42::encode(order, payload, sizeof payload);
        send_all(frame, soup::encode(soup::UnsequencedData{{payload, sizeof payload}}, frame, sizeof frame));
    }

    void send_all(const char* data, std::size_t size) {
        if (size == 0) fail("encode failed");
        std::uint64_t blocked_ns = 0;  // read the clock only once the send buffer is full
        while (size != 0) {
            const ssize_t sent = net::send_some(order_entry_.fd(), data, size);
            if (sent < 0 && !net::is_would_block(errno)) fail(std::string("send: ") + std::strerror(errno));
            if (sent > 0) {
                data += sent;
                size -= static_cast<std::size_t>(sent);
            } else {
                if (blocked_ns == 0) blocked_ns = now_ns();
                check(blocked_ns, "room in the TCP send buffer");
            }
        }
    }

    // One non-blocking recv; calls on_packet for every complete SoupBinTCP packet.
    template <class OnPacket>
    void read_order_entry(OnPacket&& on_packet) {
        const ssize_t received = net::receive_some(order_entry_.fd(), input_.data(), input_.size());
        if (received == 0) fail("the exchange closed the order entry connection");
        if (received < 0) {
            if (net::is_would_block(errno)) return;
            fail(std::string("recv: ") + std::strerror(errno));
        }
        std::span<const char> bytes(input_.data(), static_cast<std::size_t>(received));
        while (!bytes.empty()) {
            std::size_t consumed = 0;
            soup::Packet packet;
            const protocol::Status status = frames_.feed(bytes, consumed, packet);
            bytes = bytes.subspan(consumed);
            if (status == protocol::Status::incomplete) return;
            if (status != protocol::Status::ok) fail("bad SoupBinTCP frame");
            on_packet(packet);
        }
    }

    // One non-blocking recvfrom; calls on_message for every message in the
    // MoldUDP64 packet. A sequence gap (a lost datagram) fails the run.
    template <class OnMessage>
    void read_feed(OnMessage&& on_message) {
        net::Ipv4Endpoint peer{};
        const ssize_t received = feed_.receive_from(datagram_, peer);
        if (received < 0) {
            if (net::is_would_block(errno)) return;
            fail(std::string("recvfrom: ") + std::strerror(errno));
        }
        const auto size = static_cast<std::size_t>(received);
        protocol::moldudp64::Request header;  // a downstream header has the same layout
        if (size < 20 || protocol::moldudp64::decode(datagram_.data(), 20, header) != protocol::Status::ok)
            fail("short MoldUDP64 packet");
        if (header.session != feed_session_) fail("feed packet from another MoldUDP64 session");
        if (header.count == 65535) return;  // end of session
        if (header.sequence != next_feed_sequence_)
            fail("feed gap: expected sequence " + std::to_string(next_feed_sequence_) + ", got " +
                 std::to_string(header.sequence));
        std::size_t at = 20;
        for (unsigned message = 0; message < header.count; ++message) {
            const std::size_t length = size - at < 2 ? 0 : protocol::wire::load16(datagram_.data() + at);
            if (length == 0 || length > size - at - 2) fail("bad MoldUDP64 message block");
            on_message(std::span<const char>(datagram_.data() + at + 2, length));
            at += 2 + length;
        }
        if (at != size) fail("bytes after the last MoldUDP64 message block");
        next_feed_sequence_ += header.count;
    }

    net::UdpReceiver feed_;
    std::uint16_t feed_port_ = 0;
    std::array<char, 10> feed_session_{};
    std::uint64_t next_feed_sequence_ = 1;
    std::array<char, 2048> datagram_{};
    net::Socket order_entry_;
    std::array<char, 65536> input_{};
    std::array<char, 512> frame_{};
    soup::FrameReader frames_{frame_};
    std::uint64_t next_token_ = 1;
};

// Adds the pass's ack and feed rows, as the CSV and the table name them.
void add_rows(std::vector<std::pair<std::string, bench::Stats>>& rows, const char* workload, std::size_t window,
              const Pass& pass) {
    for (const auto& [metric, histogram] : {std::pair{"ack", &pass.ack}, std::pair{"feed", &pass.feed}}) {
        bench::Stats stats = bench::Stats::from(*histogram, 1);
        stats.throughput_per_second = pass.orders_per_second;
        rows.emplace_back(std::string(workload) + "_" + metric + "_w" + std::to_string(window), stats);
    }
}

}  // namespace

int main(int argc, char** argv) {
    // It starts the exchange next to it, so a bare name, found on PATH, would start the current folder's.
    if (std::string_view(argv[0]).find('/') == std::string_view::npos) {
        std::fprintf(stderr, "exsim_wire_bench: run it by its path, as build/bench/exsim_wire_bench\n");
        return 2;
    }
    // Not normalized: "link/.." must reach the same folder the OS reached for this binary.
    const std::filesystem::path directory = std::filesystem::absolute(argv[0]).parent_path();
    std::size_t orders = 50'000;
    std::size_t window = 64;
    int core = -1;
    std::string csv_path = (directory / "wire_results.csv").string();
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        bool ok = true;
        if (arg.starts_with("--orders=")) {
            // The cap keeps the config arithmetic from overflowing; the exchange checks its own limits.
            ok = parse_number(arg.substr(9), orders) && orders > 0 && orders <= 1'000'000'000;
        } else if (arg.starts_with("--window=")) {
            ok = parse_number(arg.substr(9), window) && window > 0;
        } else if (arg.starts_with("--core=")) {
            ok = parse_number(arg.substr(7), core);
        } else if (arg.starts_with("--csv=")) {
            csv_path = arg.substr(6);
        } else {
            ok = false;
        }
        if (!ok) {
            std::fprintf(stderr, "usage: %s [--orders=N] [--window=N] [--core=N] [--csv=PATH]\n", argv[0]);
            return 2;
        }
    }

    // A file opened later would take the number of a closed stdout or stderr, and the report or an error
    // would go into it.
    if (::fcntl(STDOUT_FILENO, F_GETFD) < 0 || ::fcntl(STDERR_FILENO, F_GETFD) < 0) {
        std::fprintf(stderr, "exsim_wire_bench: stdout and stderr must be open\n");
        return 1;
    }
    try {
        WireClient client;
        const std::string binary = (directory / "exsim_exchanged").string();
        const std::string config = (directory / "wire_latency.conf").string();
        if (!(std::ofstream(config) << config_text(client.feed_port(), orders))) fail("cannot write " + config);
        // After the config, so a --csv that names it replaces it and the exchange fails to start. Its header is
        // written before the handlers and the exchange exist, so a CSV whose reader stops reading holds up only
        // this process, which a signal ends. The rows wait for the exchange's exit.
        bench::CSVWriter csv(csv_path);
        // Before the spawn, so no signal can end this process with the exchange left running.
        struct sigaction action {};
        action.sa_handler = [](int number) { interrupted = number; };
        sigemptyset(&action.sa_mask);
        for (const int number : kStopSignals) ::sigaction(number, &action, nullptr);
        ExchangeProcess exchange(binary, config);
        const std::string ready = exchange.read_ready_line();
        std::uint16_t port = 0;
        if (!parse_number(ready_field(ready, "orderEntryPort"), port) || port == 0)
            fail("bad orderEntryPort in the ready line: " + ready);
        // After the spawn, so the exchange does not inherit the client's core.
        const bool pinned = core >= 0 && bench::CPUPinner::pin(core);
        client.log_in(port, ready_field(ready, "session"));

        std::vector<std::pair<std::string, bench::Stats>> rows;
        std::vector<std::size_t> windows{1};
        if (window > 1) windows.push_back(window);
        for (const std::size_t in_flight : windows) {
            const Pass rest = client.run('B', kRestPrice, orders, in_flight, itch::AddOrder::type);
            if (rest.feed_references != rest.ack_references) fail("an Add Order does not match its order");
            add_rows(rows, "rest", in_flight, rest);
            const Pass resting = client.run('S', kCrossPrice, orders, in_flight, itch::AddOrder::type);
            if (resting.feed_references != resting.ack_references) fail("an Add Order does not match its order");
            const Pass cross = client.run('B', kCrossPrice, orders, in_flight, itch::OrderExecuted::type);
            if (cross.feed_references != resting.ack_references)
                fail("an Order Executed does not name the resting order it should fill");
            add_rows(rows, "cross", in_flight, cross);
        }
        client.close();
        if (exchange.stop() != 0) fail("the exchange did not exit cleanly");
        end_on_signals();
        // No CSV row and nothing on stdout until the exchange has stopped, so a reader that stops reading cannot
        // keep it running.
        std::string table = std::format("Orders per pass: {}; client pinned by --core: {}\n", orders,
                                        pinned ? std::to_string(core) : "no");
        table += std::format("{:<16} {:>8} {:>9} {:>9} {:>9} {:>9} {:>9} {:>11}\n", "pass", "orders", "mean_ns",
                             "p50_ns", "p99_ns", "p99.9_ns", "max_ns", "orders/s");
        for (const auto& [name, stats] : rows) {
            csv.write(name, stats);
            table += std::format("{:<16} {:8} {:9.0f} {:9.0f} {:9.0f} {:9.0f} {:9.0f} {:11.0f}\n", name,
                                 stats.operations, stats.mean_ns, stats.p50_ns, stats.p99_ns, stats.p999_ns,
                                 stats.max_ns, stats.throughput_per_second);
        }
        table += std::format("Exchange: {}\nResults: {}\n", binary, csv_path);
        // One write and no stdio, so nothing is left for exit() to retry.
        if (::write(STDOUT_FILENO, table.data(), table.size()) != static_cast<ssize_t>(table.size()))
            fail("cannot write the report to stdout");
    } catch (const std::exception& error) {
        end_on_signals();  // the destructors have stopped the exchange
        std::fprintf(stderr, "exsim_wire_bench: %s\n", error.what());
        return 1;
    }
    return 0;
}
