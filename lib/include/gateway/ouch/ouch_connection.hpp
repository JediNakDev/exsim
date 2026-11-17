#ifndef EXSIM_GATEWAY_OUCH_OUCH_CONNECTION_HPP
#define EXSIM_GATEWAY_OUCH_OUCH_CONNECTION_HPP

// OuchConnection: one per TCP connection. The driving contract for the whole
// OUCH gateway is at the top of gateway/ouch/ouch_user_state.hpp.

#include "gateway/byte_sink.hpp"
#include "gateway/ouch/ouch_user_state.hpp"
#include "protocol/soupbintcp.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

namespace exsim::gateway::ouch {

// ============================================================================
// OuchConnection
// ============================================================================

template <gateway::ByteSink ByteSink>
class OuchConnection {
public:
    // Largest inbound SoupBinTCP frame accepted (Enter Order is 52 bytes;
    // longer Debug packets are a protocol violation).
    static constexpr std::size_t kInboundFrameCapacity = 256;

    // `user_states` is where logins find their user; it must outlive the connection.
    OuchConnection(ByteSink sink, const OuchUserStateDirectory<ByteSink>& user_states,
                   server::ConnectionId connection_id, const OuchSessionSettings& settings,
                   Timestamp connected_at) noexcept
        : sink_(std::move(sink)),
          user_states_(&user_states),
          connection_id_(connection_id),
          settings_(settings),
          frame_reader_(std::span<char>(inbound_frame_), kInboundFrameCapacity - 2),
          last_inbound_ns_(connected_at),
          last_outbound_ns_(connected_at) {}

    // Safety net only: the server must call on_disconnect(ctx) first so the
    // session is unbound (and cancel-on-disconnect applied).
    ~OuchConnection() {
        if (user_state_ != nullptr) user_state_->detach_connection(this);
    }

    // The user state keeps a pointer to its connection: construct in place.
    OuchConnection(const OuchConnection&) = delete;
    OuchConnection& operator=(const OuchConnection&) = delete;
    OuchConnection(OuchConnection&&) = delete;
    OuchConnection& operator=(OuchConnection&&) = delete;

    // Consumes every byte (partial frames are buffered internally).
    template <class Context>
        requires OuchServerContext<Context>
    std::size_t on_data(std::span<const char> bytes, Context& context) noexcept {
        if (state_ == OuchConnectionState::Closed) return bytes.size();
        last_inbound_ns_ = context.now();
        std::size_t offset = 0;
        while (offset < bytes.size() && state_ != OuchConnectionState::Closed) {
            std::size_t consumed = 0;
            protocol::soupbintcp::Packet packet;
            const protocol::Status status = frame_reader_.feed(bytes.subspan(offset), consumed, packet);
            offset += consumed;
            if (status == protocol::Status::incomplete) break;
            if (status != protocol::Status::ok) {
                close(OuchCloseReason::ProtocolViolation);
                break;
            }
            handle_packet(packet, context);
        }
        if (state_ == OuchConnectionState::Closed) release_session(context);
        return bytes.size();
    }

    // Idle timeout, server heartbeat, paused replay.
    template <class Context>
        requires OuchServerContext<Context>
    void on_timer(Context& context) noexcept {
        if (state_ != OuchConnectionState::Closed) {
            const Timestamp now = context.now();
            if (elapsed(last_inbound_ns_, now) >= settings_.client_idle_timeout_ns) {
                close(OuchCloseReason::ClientIdleTimeout);
            } else if (state_ == OuchConnectionState::LoggedIn) {
                deliver_stored_messages(context);
                if (state_ == OuchConnectionState::LoggedIn && !replaying_ &&
                    elapsed(last_outbound_ns_, now) >= settings_.server_heartbeat_interval_ns &&
                    !write_unsequenced(protocol::soupbintcp::ServerHeartbeat{}, now)) {
                    close(OuchCloseReason::SendBufferFull);
                }
            }
        }
        if (state_ == OuchConnectionState::Closed) release_session(context);
    }

    // Send space became available: continue a paused replay.
    template <class Context>
        requires OuchServerContext<Context>
    void on_writable(Context& context) noexcept {
        if (state_ == OuchConnectionState::LoggedIn) deliver_stored_messages(context);
        if (state_ == OuchConnectionState::Closed) release_session(context);
    }

    // The TCP connection is gone (or the server closed it after
    // close_requested()). Detaches and unbinds the user: cancel-on-disconnect.
    template <class Context>
        requires OuchServerContext<Context>
    void on_disconnect(Context& context) noexcept {
        if (state_ != OuchConnectionState::Closed) close(OuchCloseReason::TransportClosed);
        release_session(context);
    }

    // Server shutdown: SoupBinTCP End of Session, then close.
    template <class Context>
        requires OuchServerContext<Context>
    void end_session(Context& context) noexcept {
        if (state_ == OuchConnectionState::LoggedIn) deliver_stored_messages(context);
        // During a drop_out cut the client is missing messages: no End of
        // Session (it would say "do not reconnect"), just the cut.
        if (state_ == OuchConnectionState::LoggedIn && outbound_cut_by_drop_out_) close(OuchCloseReason::FaultDropOut);
        if (state_ == OuchConnectionState::LoggedIn && replaying_) close(OuchCloseReason::SendBufferFull);
        if (state_ == OuchConnectionState::LoggedIn) {
            write_unsequenced(protocol::soupbintcp::EndOfSession{}, context.now());
        }
        if (state_ != OuchConnectionState::Closed) close(OuchCloseReason::EndOfSession);
        release_session(context);
    }

    [[nodiscard]] OuchConnectionState state() const noexcept { return state_; }
    [[nodiscard]] bool close_requested() const noexcept { return state_ == OuchConnectionState::Closed; }
    [[nodiscard]] OuchCloseReason close_reason() const noexcept { return close_reason_; }
    // Send what is queued before closing, except for an overloaded connection.
    [[nodiscard]] bool flush_before_close() const noexcept {
        return close_reason_ != OuchCloseReason::SendBufferFull && close_reason_ != OuchCloseReason::TransportClosed;
    }
    [[nodiscard]] bool is_replaying() const noexcept { return replaying_; }
    [[nodiscard]] std::uint64_t next_sequence_to_send() const noexcept { return next_sequence_to_send_; }
    [[nodiscard]] server::ConnectionId connection_id() const noexcept { return connection_id_; }
    [[nodiscard]] OuchUserState<ByteSink>* user_state() const noexcept { return user_state_; }
    [[nodiscard]] ByteSink& sink() noexcept { return sink_; }
    [[nodiscard]] const ByteSink& sink() const noexcept { return sink_; }

private:
    friend class OuchUserState<ByteSink>;

    template <class Context>
    void handle_packet(const protocol::soupbintcp::Packet& packet, Context& context) noexcept {
        if (state_ == OuchConnectionState::AwaitingLogin) {
            switch (packet.type) {
                case 'L': handle_login_request(packet, context); return;
                case 'R':
                case '+': return;
                case 'O': close(OuchCloseReason::ClientLogout); return;
                default: close(OuchCloseReason::ProtocolViolation); return;
            }
        }
        switch (packet.type) {
            case 'U': user_state_->handle_application_message(packet.payload, context); return;
            case 'R':
            case '+': return;
            case 'O': close(OuchCloseReason::ClientLogout); return;
            default: close(OuchCloseReason::ProtocolViolation); return;  // second login or a server-only type
        }
    }

    template <class Context>
    void handle_login_request(const protocol::soupbintcp::Packet& packet, Context& context) noexcept {
        // The packet payload borrows the frame reader's buffer, which holds the
        // whole frame: the codec decodes Login Request from the frame start.
        protocol::soupbintcp::LoginRequest request;
        if (protocol::soupbintcp::decode(packet.payload.data() - 3, packet.payload.size() + 3, request) !=
            protocol::Status::ok) {
            close(OuchCloseReason::ProtocolViolation);
            return;
        }
        const Timestamp now = context.now();
        const server::UserConfig* user = context.authenticate(
            protocol::wire::alpha(request.username.data(), request.username.size()),
            protocol::wire::alpha(request.password.data(), request.password.size()), server::Protocol::Ouch42);
        OuchUserState<ByteSink>* user_state = user != nullptr ? user_states_->find(user->session_id) : nullptr;
        if (user_state == nullptr) {
            reject_login(kSoupLoginRejectNotAuthorized, now);
            return;
        }
        if (!requested_session_available(request.session) || user_state->store_overflowed() ||
            user_state->has_attached_connection() || !context.bind(user->session_id, connection_id_)) {
            reject_login(kSoupLoginRejectSessionNotAvailable, now);
            return;
        }
        accept_login(*user_state, request.sequence, context);
    }

    // Requested sequence 0 starts after the last stored message; N >= 1
    // replays from N, clamped to the next message to be generated.
    template <class Context>
    void accept_login(OuchUserState<ByteSink>& user_state, std::uint64_t requested_sequence,
                      Context& context) noexcept {
        const Timestamp now = context.now();
        const std::uint64_t next_unstored = user_state.store().next_seq();
        const std::uint64_t first_sequence =
            requested_sequence == 0 ? next_unstored : std::min(requested_sequence, next_unstored);
        user_state_ = &user_state;
        state_ = OuchConnectionState::LoggedIn;
        next_sequence_to_send_ = first_sequence;
        replaying_ = first_sequence < next_unstored;
        user_state.attach_connection(this);

        protocol::soupbintcp::LoginAccepted accepted;
        accepted.session = settings_.soup_session;
        accepted.sequence = first_sequence;
        if (!write_unsequenced(accepted, now)) {
            close(OuchCloseReason::SendBufferFull);
            return;
        }
        deliver_stored_messages(context);
        if (state_ == OuchConnectionState::LoggedIn) user_state.append_start_of_day_if_needed(context);
    }

    void reject_login(char reason, Timestamp now) noexcept {
        write_unsequenced(protocol::soupbintcp::LoginRejected{reason}, now);
        close(OuchCloseReason::LoginRejected);
    }

    [[nodiscard]] bool requested_session_available(const std::array<char, 10>& requested) const noexcept {
        return requested == protocol::wire::spaces<10>() || requested == settings_.soup_session;
    }

    // Writes stored messages from next_sequence_to_send_ while the sink has
    // room. A replay pauses on a full sink and continues from on_writable /
    // on_timer; live output that does not fit closes the connection
    // (SendBufferFull) and the client recovers by replay.
    template <class Context>
    void deliver_stored_messages(Context& context) noexcept {
        if (state_ != OuchConnectionState::LoggedIn) return;
        const Timestamp now = context.now();
        const SequencedStore& store = user_state_->store();
        while (next_sequence_to_send_ <= store.last_seq()) {
            // `fault user X drop_out n` is a transport cut. SoupBinTCP numbers
            // sequenced messages implicitly, so a gap in a live stream could
            // never be detected: the n messages stay stored, nothing after
            // them is written, and the connection closes after the n-th. A
            // client reconnecting with its own count + 1 replays them all.
            if (!replaying_ && user_state_->consume_outbound_drop(context)) {
                ++next_sequence_to_send_;
                outbound_cut_by_drop_out_ = true;
                if (!context.has_pending_drop_out(user_state_->session_id())) {
                    close(OuchCloseReason::FaultDropOut);
                    return;
                }
                continue;
            }
            if (outbound_cut_by_drop_out_) [[unlikely]] {  // the fault was cleared mid-cut: never write past a gap
                close(OuchCloseReason::FaultDropOut);
                return;
            }
            const SequencedStore::Message message = store.get(next_sequence_to_send_);
            char* destination = sink_.prepare(message.bytes.size());
            if (destination == nullptr) {
                if (!replaying_) close(OuchCloseReason::SendBufferFull);
                return;
            }
            std::memcpy(destination, message.bytes.data(), message.bytes.size());
            sink_.commit(message.bytes.size());
            ++next_sequence_to_send_;
            last_outbound_ns_ = now;
        }
        replaying_ = false;
    }

    // Called by the user state during dispatch: no context calls here.
    void terminate_for_store_overflow(Timestamp now) noexcept {
        if (state_ != OuchConnectionState::LoggedIn) return;
        write_unsequenced(protocol::soupbintcp::EndOfSession{}, now);
        close(OuchCloseReason::StoreFull);
    }

    template <class Message>
    bool write_unsequenced(const Message& message, Timestamp now) noexcept {
        char frame[64];
        const std::size_t frame_size = protocol::soupbintcp::encode(message, frame, sizeof frame);
        char* destination = frame_size == 0 ? nullptr : sink_.prepare(frame_size);
        if (destination == nullptr) return false;
        std::memcpy(destination, frame, frame_size);
        sink_.commit(frame_size);
        last_outbound_ns_ = now;
        return true;
    }

    void close(OuchCloseReason reason) noexcept {
        state_ = OuchConnectionState::Closed;
        close_reason_ = reason;
        replaying_ = false;
    }

    template <class Context>
    void release_session(Context& context) noexcept {
        if (user_state_ == nullptr) return;
        OuchUserState<ByteSink>* user_state = user_state_;
        user_state_ = nullptr;
        user_state->detach_connection(this);
        context.unbind(user_state->session_id(), connection_id_);
    }

    [[nodiscard]] static Timestamp elapsed(Timestamp since, Timestamp now) noexcept {
        return now > since ? now - since : 0;
    }

    ByteSink sink_;
    const OuchUserStateDirectory<ByteSink>* user_states_;
    server::ConnectionId connection_id_;
    OuchSessionSettings settings_;
    char inbound_frame_[kInboundFrameCapacity]{};
    protocol::soupbintcp::FrameReader frame_reader_;
    OuchUserState<ByteSink>* user_state_ = nullptr;  // non-null exactly while bound
    Timestamp last_inbound_ns_;
    Timestamp last_outbound_ns_;
    std::uint64_t next_sequence_to_send_ = 1;
    OuchConnectionState state_ = OuchConnectionState::AwaitingLogin;
    OuchCloseReason close_reason_ = OuchCloseReason::None;
    bool replaying_ = false;
    bool outbound_cut_by_drop_out_ = false;  // a live message was dropped: nothing sequenced is written after it
};

}  // namespace exsim::gateway::ouch

#endif
