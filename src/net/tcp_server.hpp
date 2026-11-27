#ifndef EXSIM_NET_TCP_SERVER_HPP
#define EXSIM_NET_TCP_SERVER_HPP

// TCP listener plus a pre-allocated table of connection slots, driven
// explicitly by the application's single-threaded event loop:
//
//   Poller poller;                 poller.open();
//   TcpServer server(config, poller, kOrderEntryTag);
//   server.open();
//   for (;;) {
//       poller.wait(timeout_ms);   // 0 = busy poll
//       for (const PollEvent& event : poller.events())
//           if (poll_token_tag(event.token) == kOrderEntryTag) server.handle_event(event, gateway);
//       ...engine runs, replies are encoded with reserve_send_if_room() / commit_send()...
//       server.flush_pending(gateway);
//       server.on_timer(monotonic_ns);  // in the loop's timer pass
//   }
//
// The handler is any type with these members (static dispatch, no virtuals):
//
//   void on_connection_opened(ConnectionId, Ipv4Endpoint peer);
//   void on_connection_data(ConnectionId, ReceiveBuffer& received);  // consume() whole messages
//   void on_connection_closed(ConnectionId, CloseReason, ErrorCode);
//
// Inside a callback the handler may call reserve_send_if_room(),
// commit_send(), close_now() or close_after_flush() on any connection.
//
// on_connection_closed reports the closes the application did not ask for
// (peer close, socket error, buffer limits). Closes the application requests
// with close_now() or close_after_flush() are never reported.
//
// Connection ids carry a generation counter: once a connection closes, its id
// is stale and every call with it fails or is ignored, even after the slot is
// reused by a new connection.
//
// No allocation after construction: slots, buffers and queues are sized from
// TcpServerConfig. Connections beyond max_connections are accepted and closed
// at once. Output that does not fit send_buffer_capacity is refused, and the
// caller decides what to do. The Poller must outlive the server.

#include <cstddef>
#include <cstdint>
#include <net/poller.hpp>
#include <net/socket.hpp>
#include <net/stream_buffers.hpp>
#include <net/tcp_connection.hpp>
#include <span>
#include <vector>

namespace exsim::net {

struct TcpServerConfig {
    Ipv4Endpoint listen_endpoint = loopback_endpoint(0);  // port 0 = the OS picks a free port
    std::uint32_t max_connections = 64;
    std::size_t receive_buffer_capacity = 64 * 1024;  // per connection; must hold the largest inbound message
    std::size_t send_buffer_capacity = 1024 * 1024;   // per connection; output beyond this is refused
    int listen_backlog = 128;
    int socket_send_buffer_bytes = 0;     // SO_SNDBUF of accepted sockets, 0 = OS default
    int socket_receive_buffer_bytes = 0;  // SO_RCVBUF of accepted sockets, 0 = OS default
    bool tcp_no_delay = true;
};

struct ConnectionId {
    std::uint32_t slot = 0;        // index into the server's slot table, < max_connections
    std::uint32_t generation = 0;  // 0 never names a connection

    friend constexpr bool operator==(const ConnectionId&, const ConnectionId&) = default;
};

enum class CloseReason : std::uint8_t {
    PeerClosed,         // orderly shutdown by the peer
    ConnectionError,    // recv/send/poller failure; the ErrorCode says which (ECONNRESET, EPIPE, ...)
    ReceiveBufferFull,  // the handler left the receive buffer full: an inbound message exceeds its capacity
};

template <class Handler>
concept TcpServerHandler = requires(Handler& handler, ConnectionId id, Ipv4Endpoint peer, ReceiveBuffer& received,
                                    CloseReason reason, ErrorCode error) {
    handler.on_connection_opened(id, peer);
    handler.on_connection_data(id, received);
    handler.on_connection_closed(id, reason, error);
};

class TcpServer {
public:
    // token_tag becomes the top 8 bits of every poll token this server
    // registers; route an event here when poll_token_tag(event.token) == token_tag.
    TcpServer(const TcpServerConfig& config, Poller& poller, std::uint8_t token_tag);
    ~TcpServer();
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // Binds, listens and registers the listener with the poller.
    ErrorCode open() noexcept;
    // Closes every connection and the listener. Nothing is reported.
    void close() noexcept;

    // The bound address; with port 0 in the config this holds the real port after open().
    Ipv4Endpoint listen_endpoint() const noexcept { return listen_endpoint_; }

    // Accepts on listener events; on connection events reads (one recv) and
    // delivers data, flushes on write readiness, detects peer close and errors.
    template <TcpServerHandler Handler>
    void handle_event(const PollEvent& event, Handler& handler) noexcept;

    // Sends the queued bytes of every connection written to since the last
    // call; keeps write interest registered while bytes remain; completes
    // graceful closes.
    template <TcpServerHandler Handler>
    void flush_pending(Handler& handler) noexcept;

    // From the loop's timer pass, with monotonic_ns(): watches the listener
    // again once its pause after running out of descriptors has passed, and
    // closes connections still flushing at their close deadline.
    void on_timer(std::uint64_t now_ns) noexcept;

    // True for an accepted connection that is not closing.
    bool is_open(ConnectionId id) const noexcept {
        return id.slot < slots_.size() && slots_[id.slot].state == SlotState::Open &&
               slots_[id.slot].generation == id.generation;
    }

    // Zero-copy send: encode into the returned `max_bytes` span, then
    // commit_send() the bytes used. Empty span = stale or closing connection,
    // or bytes that do not fit. The connection stays usable: the caller
    // chooses between waiting for the socket to drain (a paused replay) and
    // closing the connection itself.
    std::span<char> reserve_send_if_room(ConnectionId id, std::size_t max_bytes) noexcept {
        Slot* slot = open_slot(id);
        return slot == nullptr ? std::span<char>{} : slot->connection.send_buffer().reserve(max_bytes);
    }
    void commit_send(ConnectionId id, std::size_t bytes) noexcept {
        Slot* slot = open_slot(id);
        if (slot == nullptr) {
            return;
        }
        slot->connection.send_buffer().commit(bytes);
        queue_for_flush(id.slot);
    }

    // Stops delivering data, sends what is queued, then discards input that
    // has already arrived (at most kMaxDiscardReadsBeforeClose reads) and
    // closes the socket and frees the slot. Best effort, not a guaranteed FIN:
    // input still arriving at close() (a flooding or concurrently sending
    // peer) makes the kernel send RST, which can drop queued bytes the peer
    // has not read yet. on_timer closes a connection still flushing after
    // kCloseFlushTimeoutNs, so a peer that stops reading cannot hold its slot.
    // Returns false for a stale or closing id.
    bool close_after_flush(ConnectionId id) noexcept;
    static constexpr std::uint64_t kCloseFlushTimeoutNs = 5'000'000'000;
    // Closes immediately, dropping queued bytes. Returns false for a stale id.
    bool close_now(ConnectionId id) noexcept;

    // The connection's socket and buffers (open or closing), nullptr for a stale id.
    TcpConnection* connection(ConnectionId id) noexcept {
        Slot* slot = live_slot(id);
        return slot == nullptr ? nullptr : &slot->connection;
    }

    std::uint32_t connection_count() const noexcept { return connection_count_; }  // open + closing

private:
    enum class SlotState : std::uint8_t { Free, Open, Closing };

    struct Slot {
        Slot(std::size_t receive_capacity, std::size_t send_capacity) : connection(receive_capacity, send_capacity) {}

        TcpConnection connection;
        std::uint32_t generation = 1;
        SlotState state = SlotState::Free;
        bool write_interest = false;
        bool queued_for_flush = false;
        std::uint64_t close_deadline_ns = 0;  // Closing: on_timer closes it from here on
    };

    enum class AcceptOutcome : std::uint8_t { Accepted, Rejected, NothingPending, Failed };

    struct FlushOutcome {
        bool must_close;
        CloseReason reason;
        ErrorCode error;
    };

    // Poll token value = generation << 32 | slot index; the listener uses kListenerSlotIndex.
    static constexpr std::uint32_t kListenerSlotIndex = 0xFFFFFFFF;
    static constexpr std::uint32_t kGenerationMask = 0x00FFFFFF;  // 24 bits fit under the 8-bit owner tag
    static constexpr int kMaxAcceptsPerEvent = 64;
    static constexpr int kMaxDiscardReadsBeforeClose = 16;

    std::uint64_t slot_token(std::uint32_t slot_index) const noexcept {
        return make_poll_token(token_tag_,
                               (std::uint64_t{slots_[slot_index].generation} << 32) | std::uint64_t{slot_index});
    }

    // Slot of an open or closing connection with this exact id.
    Slot* live_slot(ConnectionId id) noexcept {
        if (id.slot >= slots_.size()) {
            return nullptr;
        }
        Slot& slot = slots_[id.slot];
        return (slot.state != SlotState::Free && slot.generation == id.generation) ? &slot : nullptr;
    }
    // Slot of an open (not closing) connection with this exact id.
    Slot* open_slot(ConnectionId id) noexcept { return is_open(id) ? &slots_[id.slot] : nullptr; }

    void queue_for_flush(std::uint32_t slot_index) noexcept {
        Slot& slot = slots_[slot_index];
        if (slot.queued_for_flush) {
            return;
        }
        slot.queued_for_flush = true;
        std::size_t tail = flush_queue_head_ + flush_queue_count_;
        if (tail >= flush_queue_.size()) {
            tail -= flush_queue_.size();
        }
        flush_queue_[tail] = slot_index;
        ++flush_queue_count_;
    }

    AcceptOutcome accept_one(ConnectionId& id_out, Ipv4Endpoint& peer_out) noexcept;
    // Sends queued bytes and syncs write interest; completes a graceful close
    // once drained. must_close = the caller closes the connection.
    FlushOutcome flush_slot(std::uint32_t slot_index) noexcept;
    void finish_graceful_close(std::uint32_t slot_index) noexcept;
    void release_slot(std::uint32_t slot_index) noexcept;

    template <class Handler>
    void close_and_report(std::uint32_t slot_index, CloseReason reason, ErrorCode error, Handler& handler) noexcept {
        Slot& slot = slots_[slot_index];
        const bool application_requested_close = slot.state == SlotState::Closing;
        const ConnectionId id{slot_index, slot.generation};
        release_slot(slot_index);
        if (!application_requested_close) {
            handler.on_connection_closed(id, reason, error);
        }
    }

    TcpServerConfig config_;
    Poller& poller_;
    std::uint8_t token_tag_;
    Socket listener_;
    Ipv4Endpoint listen_endpoint_{};

    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_slot_stack_;  // LIFO of free slot indices
    std::uint32_t free_slot_count_ = 0;
    std::vector<std::uint32_t> flush_queue_;  // ring of slot indices; each slot queued at most once
    std::size_t flush_queue_head_ = 0;
    std::size_t flush_queue_count_ = 0;

    std::uint32_t connection_count_ = 0;
    std::uint64_t accept_paused_until_ns_ = 0;  // 0 = the listener is watched
    std::uint32_t closing_count_ = 0;           // on_timer scans the slots only when nonzero
};

template <TcpServerHandler Handler>
void TcpServer::handle_event(const PollEvent& event, Handler& handler) noexcept {
    if (poll_token_tag(event.token) != token_tag_ || !listener_.is_open()) return;
    const std::uint64_t token_value = poll_token_value(event.token);
    const auto slot_index = static_cast<std::uint32_t>(token_value);

    if (slot_index == kListenerSlotIndex) {
        for (int attempt = 0; attempt < kMaxAcceptsPerEvent; ++attempt) {
            ConnectionId id{};
            Ipv4Endpoint peer{};
            const AcceptOutcome outcome = accept_one(id, peer);
            if (outcome == AcceptOutcome::NothingPending || outcome == AcceptOutcome::Failed) {
                return;
            }
            if (outcome == AcceptOutcome::Accepted) {
                handler.on_connection_opened(id, peer);
            }
        }
        return;
    }

    const ConnectionId id{slot_index, static_cast<std::uint32_t>(token_value >> 32)};
    Slot* slot = live_slot(id);
    if (slot == nullptr) {
        return;  // the connection closed earlier in this batch of events
    }

    if (slot->state == SlotState::Closing) {
        const FlushOutcome outcome = flush_slot(slot_index);
        if (outcome.must_close) close_and_report(slot_index, outcome.reason, outcome.error, handler);
        return;
    }

    if (event.readable || event.hangup || event.error) {
        const ReceiveResult received = slot->connection.receive();
        switch (received.status) {
            case ReceiveStatus::Received:
                if (slot->state == SlotState::Open) {
                    handler.on_connection_data(id, slot->connection.receive_buffer());
                } else {
                    slot->connection.receive_buffer().clear();  // closing: inbound data is ignored
                }
                if (live_slot(id) != nullptr && slot->state == SlotState::Open &&
                    slot->connection.receive_buffer().size() == slot->connection.receive_buffer().capacity()) {
                    close_and_report(slot_index, CloseReason::ReceiveBufferFull, ENOBUFS, handler);
                    return;
                }
                break;
            case ReceiveStatus::WouldBlock: break;
            case ReceiveStatus::PeerClosed: close_and_report(slot_index, CloseReason::PeerClosed, 0, handler); return;
            case ReceiveStatus::BufferFull:
                close_and_report(slot_index, CloseReason::ReceiveBufferFull, 0, handler);
                return;
            case ReceiveStatus::Error:
                close_and_report(slot_index, CloseReason::ConnectionError, received.error, handler);
                return;
        }
    }

    if (event.writable && live_slot(id) != nullptr) {  // the data callback may have closed it
        const FlushOutcome outcome = flush_slot(slot_index);
        if (outcome.must_close) {
            close_and_report(slot_index, outcome.reason, outcome.error, handler);
        }
    }
}

template <TcpServerHandler Handler>
void TcpServer::flush_pending(Handler& handler) noexcept {
    // Callbacks from close_and_report may queue more slots; the loop drains them too.
    while (flush_queue_count_ != 0) {
        const std::uint32_t slot_index = flush_queue_[flush_queue_head_];
        flush_queue_head_ = (flush_queue_head_ + 1 == flush_queue_.size()) ? 0 : flush_queue_head_ + 1;
        --flush_queue_count_;

        Slot& slot = slots_[slot_index];
        slot.queued_for_flush = false;
        if (slot.state == SlotState::Free) {
            continue;
        }
        const FlushOutcome outcome = flush_slot(slot_index);
        if (outcome.must_close) {
            close_and_report(slot_index, outcome.reason, outcome.error, handler);
        }
    }
}

}  // namespace exsim::net

#endif
