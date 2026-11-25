#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <common/clocks.hpp>
#include "test/exsim/support/clients/tcp_client.hpp"
#include <net/tcp_server.hpp>
#include <net/udp.hpp>
#include <vector>

#include "test/framework/test.hpp"

using namespace exsim::common;
using namespace exsim::net;
namespace {
constexpr std::uint64_t kTimeout = 5 * kNanosecondsPerSecond;
template <class Predicate, class Step>
void until(Predicate predicate, Step step) {
    const auto deadline = monotonic_ns() + kTimeout;
    bool done = predicate();
    while (!done && monotonic_ns() < deadline) {
        step();
        done = predicate();
    }
    REQUIRE(done);
}
// Queues non-empty `bytes` the way the gateways do; false for a stale or
// closing connection, or when the bytes do not fit.
bool queue(TcpServer& server, ConnectionId id, std::span<const char> bytes) {
    const std::span<char> space = server.reserve_send_if_room(id, bytes.size());
    if (space.empty()) return false;
    std::ranges::copy(bytes, space.begin());
    server.commit_send(id, bytes.size());
    return true;
}
struct Handler {
    TcpServer& server;
    std::array<ConnectionId, 128> ids{};
    std::size_t opened = 0, closed = 0, bytes = 0;
    CloseReason reason = CloseReason::PeerClosed;
    int error = 0;
    bool echo = false, consume = true, lines = false;
    void on_connection_opened(ConnectionId id, Ipv4Endpoint) noexcept { ids[opened++] = id; }
    void on_connection_closed(ConnectionId, CloseReason why, ErrorCode code) noexcept {
        ++closed;
        reason = why;
        error = code;
    }
    void on_connection_data(ConnectionId id, ReceiveBuffer& buffer) noexcept {
        auto data = buffer.readable();
        std::size_t count = data.size();
        if (lines) {
            count = 0;
            for (std::size_t index = 0; index < data.size(); ++index)
                if (data[index] == '\n') count = index + 1;
        }
        bytes += count;
        if (echo && count != 0) queue(server, id, data.first(count));
        if (consume) buffer.consume(count);
    }
};
void pump(Poller& poller, TcpServer& server, Handler& handler) {
    REQUIRE(poller.wait(1) >= 0);
    for (const auto& event : poller.events()) server.handle_event(event, handler);
    server.flush_pending(handler);
}
// Reads until the server's FIN, checking each byte against `expected`, and
// returns the byte count. The deadline restarts whenever bytes arrive and is
// checked only after a pass found none: on a loaded machine the thread can be
// descheduled for seconds, so only a stalled transfer fails.
std::size_t receive_until_eof(Poller& poller, TcpServer& server, Handler& handler, TcpClient& client,
                              std::span<const char> expected) {
    std::size_t received = 0;
    for (auto deadline = monotonic_ns() + kTimeout;;) {
        pump(poller, server, handler);
        const auto result = client.receive();
        auto data = client.receive_buffer().readable();
        REQUIRE(received + data.size() <= expected.size());
        if (!data.empty()) {
            REQUIRE_EQ(std::memcmp(data.data(), expected.data() + received, data.size()), 0);
            deadline = monotonic_ns() + kTimeout;
        }
        received += data.size();
        client.receive_buffer().consume(data.size());
        if (result.status == ReceiveStatus::PeerClosed) return received;
        REQUIRE(result.status != ReceiveStatus::Error);
        REQUIRE(monotonic_ns() < deadline);
    }
}
void connect_client(TcpClient& client, Ipv4Endpoint remote) {
    const auto error = client.connect(remote);
    REQUIRE(error == 0 || error == EINPROGRESS);
    Poller poller;
    REQUIRE_EQ(poller.open(), 0);
    REQUIRE_EQ(poller.add(client.fd(), 999, PollInterest::Write), 0);
    until([&] { return client.connected(); },
          [&] {
              REQUIRE(poller.wait(1) >= 0);
              if (!poller.events().empty()) REQUIRE_EQ(client.complete_connect(), 0);
          });
}
struct Pair {
    Socket listener, accepted;
    TcpClient client;
    Ipv4Endpoint peer{};  // the client's address, as accept_connection reported it
    Pair() {
        REQUIRE_EQ(open_tcp_listener(loopback_endpoint(0), 8, 0, 0, listener), 0);
        Ipv4Endpoint local{};
        REQUIRE_EQ(local_endpoint(listener.fd(), local), 0);
        connect_client(client, local);
        until([&] { return accepted.is_open(); },
              [&] {
                  const auto error = accept_connection(listener.fd(), accepted, peer);
                  REQUIRE(error == 0 || is_would_block(error));
              });
    }
};
}  // namespace
TEST_CASE(net_socket, ownership_and_options) {
    Socket socket;
    REQUIRE_EQ(open_tcp_socket(socket), 0);
    const int fd = socket.fd();
    CHECK(fcntl(fd, F_GETFL) & O_NONBLOCK);
    CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
    REQUIRE_EQ(set_tcp_no_delay(fd, true), 0);
    REQUIRE_EQ(set_reuse_address(fd, true), 0);
    REQUIRE_EQ(set_send_buffer_size(fd, 8192), 0);
    REQUIRE_EQ(set_receive_buffer_size(fd, 8192), 0);
    int bytes = 0;
    socklen_t length = sizeof(bytes);
    REQUIRE_EQ(getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, &length), 0);
    CHECK_GE(bytes, 8192);
    REQUIRE_EQ(getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, &length), 0);
    CHECK_GE(bytes, 8192);
    Socket moved(std::move(socket));
    CHECK(!socket.is_open());
    CHECK_EQ(moved.fd(), fd);
    Socket assigned;
    assigned = std::move(moved);
    CHECK(!moved.is_open());
    assigned.close();
    CHECK_EQ(fcntl(fd, F_GETFD), -1);
    CHECK_EQ(errno, EBADF);
    std::uint32_t address = 0;
    CHECK(parse_ipv4_address("127.0.0.1", address));
    CHECK_EQ(address, kIpv4LoopbackAddress);
    CHECK(!parse_ipv4_address("1.2.3.256", address));
    CHECK(!parse_ipv4_address("1.2.3.4.5", address));
    std::array<char, kIpv4EndpointTextCapacity> text{};
    REQUIRE(format_ipv4_endpoint(loopback_endpoint(65535), text) > 0);
    CHECK_EQ(std::string_view(text.data()), "127.0.0.1:65535");
}
TEST_CASE(net_tcp, connect_completion_and_failure) {
    Pair pair;
    CHECK(pair.client.connected());
    CHECK_EQ(pair.client.complete_connect(), 0);
    CHECK(fcntl(pair.accepted.fd(), F_GETFL) & O_NONBLOCK);
    CHECK(fcntl(pair.accepted.fd(), F_GETFD) & FD_CLOEXEC);
    Ipv4Endpoint local{};
    REQUIRE_EQ(local_endpoint(pair.client.fd(), local), 0);
    CHECK_EQ(pair.peer, local);
    Socket unlistened;
    REQUIRE_EQ(open_tcp_socket(unlistened), 0);
    REQUIRE_EQ(bind_ipv4(unlistened.fd(), loopback_endpoint(0)), 0);
    REQUIRE_EQ(local_endpoint(unlistened.fd(), local), 0);
    unlistened.close();
    TcpClient failed;
    auto error = failed.connect(local);
    if (error == EINPROGRESS) {
        Poller poller;
        REQUIRE_EQ(poller.open(), 0);
        REQUIRE_EQ(poller.add(failed.fd(), 1, PollInterest::Write), 0);
        until([&] { return error != EINPROGRESS; },
              [&] {
                  REQUIRE(poller.wait(1) >= 0);
                  if (!poller.events().empty()) error = failed.complete_connect();
              });
    }
    CHECK_EQ(error, ECONNREFUSED);
    CHECK_EQ(failed.fd(), -1);
}
TEST_CASE(net_poller, tokens_interest_and_removal) {
    Pair pair;
    Poller poller;
    REQUIRE_EQ(poller.open(), 0);
    constexpr std::uint64_t token = 0xFEDCBA9876543210ULL;
    REQUIRE_EQ(poller.add(pair.accepted.fd(), token, PollInterest::Read), 0);
    CHECK_EQ(poller.wait(0), 0);
    REQUIRE_EQ(send_some(pair.client.fd(), "abc", 3), 3);
    until([&] { return !poller.events().empty(); }, [&] { REQUIRE(poller.wait(1) >= 0); });
    CHECK_EQ(poller.events()[0].token, token);
    CHECK(poller.events()[0].readable);
    REQUIRE_EQ(poller.modify(pair.accepted.fd(), 17, PollInterest::Write), 0);
    REQUIRE(poller.wait(50) > 0);
    for (const auto& event : poller.events()) {
        CHECK_EQ(event.token, 17U);
        CHECK(event.writable);
    }
    REQUIRE_EQ(poller.remove(pair.accepted.fd()), 0);
    CHECK_EQ(poller.wait(0), 0);
    CHECK_EQ(poller.wait(-2), -1);
    CHECK(poller.events().empty());
}
TEST_CASE(net_buffers, compaction_overflow_and_single_byte_capacity) {
    ReceiveBuffer buffer(8);
    auto tail = buffer.writable_tail();
    std::memcpy(tail.data(), "abcdefgh", 8);
    buffer.commit(8);
    buffer.consume(5);
    tail = buffer.writable_tail();
    REQUIRE_EQ(tail.size(), 5U);
    std::memcpy(tail.data(), "ijklm", 5);
    buffer.commit(5);
    CHECK_EQ(std::string_view(buffer.readable().data(), buffer.size()), "fghijklm");
    CHECK_EQ(buffer.receive_from(-1).status, ReceiveStatus::BufferFull);
    ReceiveBuffer tiny(1);
    tiny.writable_tail()[0] = 'x';
    tiny.commit(1);
    tiny.consume(1);
    CHECK_EQ(tiny.writable_tail().size(), 1U);
    SendBuffer send(4);
    REQUIRE_EQ(send.reserve(4).size(), 4U);
    send.commit(4);
    CHECK(send.reserve(1).empty());
    CHECK_EQ(send.pending().size(), 4U);
    send.clear();
    CHECK_EQ(send.reserve(2).size(), 2U);
}
TEST_CASE(net_server, echo_and_split_frames) {
    Poller poller;
    REQUIRE_EQ(poller.open(), 0);
    TcpServerConfig config;
    config.receive_buffer_capacity = 8;
    TcpServer server(config, poller, 1);
    REQUIRE_EQ(server.open(), 0);
    Handler handler{server};
    handler.echo = true;
    handler.lines = true;
    TcpClient client;
    connect_client(client, server.listen_endpoint());
    until([&] { return handler.opened == 1; }, [&] { pump(poller, server, handler); });
    REQUIRE(client.send(std::span("hel", 3)));
    client.flush();
    until([&] { return server.connection(handler.ids[0])->receive_buffer().size() == 3; },
          [&] { pump(poller, server, handler); });
    CHECK_EQ(handler.bytes, 0U);
    REQUIRE(client.send(std::span("lo\nx", 4)));
    client.flush();
    until([&] { return client.receive_buffer().size() == 6; },
          [&] {
              pump(poller, server, handler);
              client.receive();
          });
    auto data = client.receive_buffer().readable();
    CHECK_EQ(std::string_view(data.data(), data.size()), "hello\n");
    CHECK_EQ(server.connection(handler.ids[0])->receive_buffer().size(), 1U);
}
TEST_CASE(net_server, connection_limit_and_rejection) {
    Poller poller;
    REQUIRE_EQ(poller.open(), 0);
    TcpServerConfig config;
    config.max_connections = 32;
    TcpServer server(config, poller, 1);
    REQUIRE_EQ(server.open(), 0);
    Handler handler{server};
    std::vector<TcpClient> clients;
    clients.reserve(33);
    for (int index = 0; index < 33; ++index) {
        clients.emplace_back(1024, 1024);
        connect_client(clients.back(), server.listen_endpoint());
    }
    // The 33rd connection is accepted and closed at once: its client sees EOF.
    bool eof = false;
    until([&] { return handler.opened == 32 && eof; },
          [&] {
              pump(poller, server, handler);
              eof = eof || clients.back().receive().status == ReceiveStatus::PeerClosed;
          });
    CHECK_EQ(server.connection_count(), 32U);
}
TEST_CASE(net_server, stale_id_event_and_flush_entry_after_reuse) {
    Poller poller;
    REQUIRE_EQ(poller.open(), 0);
    TcpServerConfig config;
    config.max_connections = 1;
    TcpServer server(config, poller, 3);
    REQUIRE_EQ(server.open(), 0);
    Handler handler{server};
    TcpClient first;
    connect_client(first, server.listen_endpoint());
    until([&] { return handler.opened == 1; }, [&] { pump(poller, server, handler); });
    const auto old = handler.ids[0];
    REQUIRE(queue(server, old, std::span("discard", 7)));
    REQUIRE(server.close_now(old));
    TcpClient second;
    connect_client(second, server.listen_endpoint());
    // Dispatch accept without flushing the old queue entry.
    until([&] { return handler.opened == 2; },
          [&] {
              REQUIRE(poller.wait(1) >= 0);
              for (const auto& event : poller.events()) server.handle_event(event, handler);
          });
    const auto current = handler.ids[1];
    CHECK_EQ(old.slot, current.slot);
    CHECK_NE(old.generation, current.generation);
    CHECK(!queue(server, old, std::span("bad", 3)));
    CHECK(!server.close_now(old));
    CHECK(server.connection(old) == nullptr);
    server.handle_event({make_poll_token(3, (std::uint64_t{old.generation} << 32) | old.slot), true, true, true, true},
                        handler);
    REQUIRE(queue(server, current, std::span("new", 3)));
    server.flush_pending(handler);
    until([&] { return second.receive_buffer().size() == 3; },
          [&] {
              second.receive();
              pump(poller, server, handler);
          });
    CHECK_EQ(std::string_view(second.receive_buffer().readable().data(), 3), "new");
}
TEST_CASE(net_server, peer_close_and_reset) {
    for (bool reset : {false, true}) {
        Poller poller;
        REQUIRE_EQ(poller.open(), 0);
        TcpServer server({}, poller, 1);
        REQUIRE_EQ(server.open(), 0);
        Handler handler{server};
        TcpClient client;
        connect_client(client, server.listen_endpoint());
        until([&] { return handler.opened == 1; }, [&] { pump(poller, server, handler); });
        if (reset) {
            linger value{1, 0};
            REQUIRE_EQ(setsockopt(client.fd(), SOL_SOCKET, SO_LINGER, &value, sizeof(value)), 0);
        }
        client.close();
        until([&] { return handler.closed == 1; }, [&] { pump(poller, server, handler); });
        CHECK_EQ(handler.reason, reset ? CloseReason::ConnectionError : CloseReason::PeerClosed);
        if (reset) CHECK_EQ(handler.error, ECONNRESET);
        CHECK_EQ(server.connection_count(), 0U);
    }
}
TEST_CASE(net_server, receive_exhaustion) {
    Poller poller;
    REQUIRE_EQ(poller.open(), 0);
    TcpServerConfig config;
    config.receive_buffer_capacity = 4;
    TcpServer server(config, poller, 1);
    REQUIRE_EQ(server.open(), 0);
    Handler handler{server};
    handler.consume = false;
    TcpClient client;
    connect_client(client, server.listen_endpoint());
    until([&] { return handler.opened == 1; }, [&] { pump(poller, server, handler); });
    REQUIRE(client.send(std::span("full", 4)));
    client.flush();
    until([&] { return handler.closed == 1; }, [&] { pump(poller, server, handler); });
    CHECK_EQ(handler.reason, CloseReason::ReceiveBufferFull);
}
TEST_CASE(net_server, reserve_if_room_refuses_without_overflowing) {
    Poller poller;
    REQUIRE_EQ(poller.open(), 0);
    TcpServerConfig config;
    config.send_buffer_capacity = 8;
    TcpServer server(config, poller, 1);
    REQUIRE_EQ(server.open(), 0);
    Handler handler{server};
    TcpClient client;
    connect_client(client, server.listen_endpoint());
    until([&] { return handler.opened == 1; }, [&] { pump(poller, server, handler); });
    const auto id = handler.ids[0];
    auto first = server.reserve_send_if_room(id, 6);
    REQUIRE_EQ(first.size(), 6U);
    std::memcpy(first.data(), "abcdef", 6);
    server.commit_send(id, 6);
    // Four more bytes do not fit: refused, but nothing queued is lost and the
    // connection stays open.
    CHECK(server.reserve_send_if_room(id, 4).empty());
    CHECK_EQ(server.connection(id)->send_buffer().pending().size(), 6U);
    std::size_t received = 0;
    until([&] { return received == 6; },
          [&] {
              pump(poller, server, handler);
              (void)client.receive();
              received = client.receive_buffer().size();
          });
    CHECK_EQ(std::string_view(client.receive_buffer().readable().data(), 6), "abcdef");
    auto second = server.reserve_send_if_room(id, 8);
    REQUIRE_EQ(second.size(), 8U);
    server.commit_send(id, 8);
    pump(poller, server, handler);
    CHECK(server.is_open(id));
    CHECK_EQ(handler.closed, 0U);
}
TEST_CASE(net_server, backpressure_order_and_graceful_close) {
    Poller poller;
    REQUIRE_EQ(poller.open(), 0);
    TcpServerConfig config;
    config.max_connections = 1;
    config.send_buffer_capacity = 4 * 1024 * 1024;
    config.socket_send_buffer_bytes = 4096;
    TcpServer server(config, poller, 1);
    REQUIRE_EQ(server.open(), 0);
    Handler handler{server};
    TcpClient client(65536, 1024);
    connect_client(client, server.listen_endpoint());
    until([&] { return handler.opened == 1; }, [&] { pump(poller, server, handler); });
    std::vector<char> expected(config.send_buffer_capacity);
    for (std::size_t index = 0; index < expected.size(); ++index) expected[index] = static_cast<char>(index % 113);
    const auto id = handler.ids[0];
    auto output = server.reserve_send_if_room(id, expected.size());
    REQUIRE_EQ(output.size(), expected.size());
    std::memcpy(output.data(), expected.data(), expected.size());
    server.commit_send(id, expected.size());
    server.flush_pending(handler);
    REQUIRE(server.connection(id) != nullptr);
    CHECK(server.connection(id)->send_buffer().has_pending());
    const auto sent = expected.size() - server.connection(id)->send_buffer().pending().size();
    REQUIRE(sent > 0);
    // One byte more than the freed prefix does not fit: refused without moving
    // the queued bytes.
    const char* queued = server.connection(id)->send_buffer().pending().data();
    CHECK(server.reserve_send_if_room(id, sent + 1).empty());
    CHECK(server.connection(id)->send_buffer().pending().data() == queued);
    const auto original_size = expected.size();
    expected.resize(original_size + sent);
    for (std::size_t index = original_size; index < expected.size(); ++index)
        expected[index] = static_cast<char>(index % 113);
    // Refill the freed prefix, forcing send-buffer compaction after a short write.
    REQUIRE(queue(server, id, std::span(expected).subspan(original_size)));
    REQUIRE_EQ(shutdown(client.fd(), SHUT_WR), 0);
    REQUIRE(server.close_after_flush(id));
    CHECK(!server.is_open(id));
    CHECK(!queue(server, id, std::span("x", 1)));
    CHECK_EQ(receive_until_eof(poller, server, handler, client, expected), expected.size());
    CHECK_EQ(server.connection_count(), 0U);
    CHECK_EQ(handler.closed, 0U);
}
// A peer that keeps sending while the server closes gracefully: close() with
// unread input sends RST, and Linux then discards queued output the peer has
// not received. The server must discard the input first so the output ends
// with a FIN.
TEST_CASE(net_server, graceful_close_with_unread_input_delivers_queued_output) {
    Poller poller;
    REQUIRE_EQ(poller.open(), 0);
    TcpServerConfig config;
    config.max_connections = 1;
    config.send_buffer_capacity = 1024 * 1024;
    config.socket_send_buffer_bytes = 4096;
    TcpServer server(config, poller, 1);
    REQUIRE_EQ(server.open(), 0);
    Handler handler{server};
    TcpClient client(65536, 1024);
    connect_client(client, server.listen_endpoint());
    until([&] { return handler.opened == 1; }, [&] { pump(poller, server, handler); });
    const auto id = handler.ids[0];
    std::vector<char> expected(config.send_buffer_capacity);
    for (std::size_t index = 0; index < expected.size(); ++index) expected[index] = static_cast<char>(index % 101);
    REQUIRE(queue(server, id, expected));
    REQUIRE(server.close_after_flush(id));
    server.flush_pending(handler);
    REQUIRE(server.connection(id) != nullptr);  // still flushing: the client has not read yet
    // The closing connection no longer reads: these bytes stay unread.
    REQUIRE(client.send(std::span("heartbeat", 9)));
    REQUIRE(client.flush().status == FlushStatus::Complete);
    CHECK_EQ(receive_until_eof(poller, server, handler, client, expected), expected.size());
    CHECK_EQ(server.connection_count(), 0U);
}
// A client that stops reading keeps a closing connection's output queued. Its
// half-close must not wake the loop (a closing slot only waits to write), and
// the timer pass frees the slot at its flush deadline.
TEST_CASE(net_server, closing_slot_of_a_stalled_peer_stays_quiet_and_expires) {
    Poller poller;
    REQUIRE_EQ(poller.open(), 0);
    TcpServerConfig config;
    config.max_connections = 1;
    config.send_buffer_capacity = 4 * 1024 * 1024;
    config.socket_send_buffer_bytes = 4096;
    TcpServer server(config, poller, 1);
    REQUIRE_EQ(server.open(), 0);
    Handler handler{server};
    TcpClient client(65536, 1024);  // never reads
    connect_client(client, server.listen_endpoint());
    until([&] { return handler.opened == 1; }, [&] { pump(poller, server, handler); });
    const auto id = handler.ids[0];
    std::vector<char> output(config.send_buffer_capacity, 'x');
    REQUIRE(queue(server, id, output));
    const std::uint64_t before_close = monotonic_ns();
    REQUIRE(server.close_after_flush(id));
    const std::uint64_t after_close = monotonic_ns();
    server.flush_pending(handler);
    REQUIRE(server.connection(id) != nullptr);  // still flushing
    // Writes continue until the client's receive window is full.
    until([&] { return poller.wait(10) == 0; },
          [&] {
              for (const auto& event : poller.events()) server.handle_event(event, handler);
          });
    REQUIRE_EQ(shutdown(client.fd(), SHUT_WR), 0);
    // A late ACK may still bring write readiness; any other event is the
    // half-close, which Linux reported on every wait before the fix.
    int other_events = 0;
    for (int wait = 0; wait < 20; ++wait) {
        REQUIRE(poller.wait(10) >= 0);
        for (const auto& event : poller.events()) {
            if (!event.writable) ++other_events;
            server.handle_event(event, handler);
        }
        server.flush_pending(handler);
    }
    CHECK_EQ(other_events, 0);
    // close_after_flush set the deadline between these two readings.
    server.on_timer(before_close + TcpServer::kCloseFlushTimeoutNs - 1);
    CHECK_EQ(server.connection_count(), 1U);
    server.on_timer(after_close + TcpServer::kCloseFlushTimeoutNs);
    CHECK_EQ(server.connection_count(), 0U);
    CHECK_EQ(handler.closed, 0U);  // an application close is not reported
}
TEST_CASE(net_tcp, broken_pipe_is_sigpipe_safe) {
    Pair pair;
    pair.accepted.close();
    std::array<char, 16> bytes{};
    bool failed = false;
    until([&] { return failed; },
          [&] {
              const auto result = send_some(pair.client.fd(), bytes.data(), bytes.size());
              if (result < 0 && !is_would_block(errno)) failed = true;
          });
}
TEST_CASE(net_udp, unicast_reply_and_truncation) {
    UdpReceiver receiver;
    REQUIRE_EQ(receiver.open(loopback_endpoint(0)), 0);
    UdpReceiver requester;
    REQUIRE_EQ(requester.open(loopback_endpoint(0)), 0);
    Ipv4Endpoint destination{}, peer{}, source{};
    REQUIRE_EQ(local_endpoint(receiver.fd(), destination), 0);
    REQUIRE_EQ(local_endpoint(requester.fd(), source), 0);
    REQUIRE_EQ(requester.send_to(std::span("request", 7), destination), 7);
    std::array<char, 64> bytes{};
    ssize_t count = -1;
    until([&] { return count >= 0; },
          [&] {
              count = receiver.receive_from(bytes, peer);
              if (count < 0) REQUIRE(is_would_block(errno));
          });
    CHECK_EQ(count, 7);
    CHECK_EQ(peer, source);
    REQUIRE_EQ(receiver.send_to(std::span("reply", 5), peer), 5);
    until([&] { return (count = requester.receive_from(bytes, peer)) >= 0; }, [] {});
    CHECK_EQ(count, 5);
    CHECK_EQ(peer, destination);
    REQUIRE_EQ(requester.send_to(bytes, destination), 64);
    bool truncated = false;
    until([&] { return truncated; },
          [&] {
              count = receiver.receive_from(std::span(bytes).first(2), peer);
              truncated = count < 0 && errno == EMSGSIZE;
              if (!truncated) REQUIRE(is_would_block(errno));
          });
    UdpSender sender;
    REQUIRE_EQ(sender.open(), 0);
    REQUIRE_EQ(sender.send_to({}, destination), 0);
    until([&] { return receiver.receive_from(bytes, peer) == 0; }, [] {});
}
TEST_CASE(net_udp, multicast_loopback) {
    UdpReceiver receiver;
    UdpSender sender;
    int error = receiver.open({kIpv4AnyAddress, 0});
    Ipv4Endpoint destination{};
    constexpr std::uint32_t group = 0xEFFF2A01;
    if (!error) error = local_endpoint(receiver.fd(), destination);
    const ip_mreq membership{{htonl(group)}, {htonl(kIpv4LoopbackAddress)}};
    if (!error && setsockopt(receiver.fd(), IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) < 0)
        error = errno;
    if (!error) error = sender.open();
    if (!error) error = sender.multicast_options(1, true, kIpv4LoopbackAddress);
    if (error) {
        std::printf("multicast unavailable, skipped (errno %d) ", error);
        return;
    }
    destination.address = group;
    if (sender.send_to(std::span("feed", 4), destination) < 0) {
        std::printf("multicast send unavailable, skipped (errno %d) ", errno);
        return;
    }
    Poller poller;
    REQUIRE_EQ(poller.open(), 0);
    REQUIRE_EQ(poller.add(receiver.fd(), 1, PollInterest::Read), 0);
    if (poller.wait(100) == 0) {
        std::printf("multicast loopback unavailable, skipped ");
        return;
    }
    std::array<char, 64> data{};
    Ipv4Endpoint peer{};
    REQUIRE_EQ(receiver.receive_from(data, peer), 4);
    CHECK_EQ(std::string_view(data.data(), 4), "feed");
}
TEST_CASE(net_clocks, epoch_midnight_and_monotonic) {
    const auto before = monotonic_ns();
    CHECK_GE(monotonic_ns(), before);
    CHECK_GT(realtime_ns(), 1700000000ULL * kNanosecondsPerSecond);
    LocalClock clock;
    clock.refresh();
    const auto now = realtime_ns();
    const auto seconds = static_cast<std::time_t>(now / kNanosecondsPerSecond);
    std::tm local{};
    localtime_r(&seconds, &local);
    CHECK_EQ(
        clock.midnight_ns(now),
        static_cast<std::uint64_t>(local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec) * kNanosecondsPerSecond +
            now % kNanosecondsPerSecond);
}

TEST_CASE(net_tcp, client_buffer_exhaustion_and_reconnect) {
    Socket listener;
    REQUIRE_EQ(open_tcp_listener(loopback_endpoint(0), 8, 0, 0, listener), 0);
    Ipv4Endpoint local{}, peer{};
    REQUIRE_EQ(local_endpoint(listener.fd(), local), 0);
    TcpClient client(4, 4);
    connect_client(client, local);
    Socket accepted;
    until([&] { return accepted.is_open(); },
          [&] {
              const auto error = accept_connection(listener.fd(), accepted, peer);
              REQUIRE(error == 0 || is_would_block(error));
          });
    const int first_fd = client.fd();
    CHECK(!client.send(std::span("excess", 6)));
    CHECK_EQ(client.fd(), -1);
    CHECK_EQ(fcntl(first_fd, F_GETFD), -1);
    accepted.close();
    connect_client(client, local);
    until([&] { return accepted.is_open(); },
          [&] {
              const auto error = accept_connection(listener.fd(), accepted, peer);
              REQUIRE(error == 0 || is_would_block(error));
          });
    REQUIRE_EQ(send_some(accepted.fd(), "full", 4), 4);
    until([&] { return client.receive_buffer().size() == 4; }, [&] { client.receive(); });
    CHECK_EQ(client.receive().status, ReceiveStatus::BufferFull);
    CHECK(!client.connected());
    CHECK_EQ(client.fd(), -1);
    CHECK_EQ(client.receive_buffer().size(), 4U);
}
TEST_CASE(net_server, reopen_and_foreign_events) {
    Poller poller;
    REQUIRE_EQ(poller.open(), 0);
    TcpServerConfig config;
    config.max_connections = 1;
    TcpServer server(config, poller, 1);
    REQUIRE_EQ(server.open(), 0);
    Handler handler{server};
    TcpClient client;
    connect_client(client, server.listen_endpoint());
    until([&] { return handler.opened == 1; }, [&] { pump(poller, server, handler); });
    const auto old = handler.ids[0];
    REQUIRE(queue(server, old, std::span("queued", 6)));
    server.close();
    CHECK_EQ(server.connection_count(), 0U);
    REQUIRE_EQ(server.open(), 0);
    connect_client(client, server.listen_endpoint());
    server.handle_event({make_poll_token(2, 0xFFFFFFFF), true, false, false, false}, handler);
    CHECK_EQ(handler.opened, 1U);
    until([&] { return handler.opened == 2; }, [&] { pump(poller, server, handler); });
    CHECK(!server.is_open(old));
    CHECK_NE(old.generation, handler.ids[1].generation);
}
