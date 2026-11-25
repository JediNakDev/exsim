#ifndef EXSIM_GATEWAY_OUCH_OUCH_USER_STATE_HPP
#define EXSIM_GATEWAY_OUCH_OUCH_USER_STATE_HPP

// Server side of OUCH 4.2 over SoupBinTCP 3.0. Pure state machines over byte
// buffers:
//
//   OuchUserState<Sink>           one per configured OUCH user for the whole
//                                 trading day: token table, per-order
//                                 instructions, sequenced store, OUCH
//                                 application mapping both ways.
//   OuchUserStateDirectory<Sink>  every OUCH user's state for the day.
//   OuchConnection<Sink>          one per TCP connection: SoupBinTCP framing,
//                                 login, replay, heartbeats, idle timeout,
//                                 logout.
//
// All are templated on the byte sink (byte_sink.hpp); their entry points are
// templated on the server Context (OuchServerContext below), which
// server::ExchangeCore satisfies. No virtual dispatch, no exceptions, no
// std::string and no heap allocation after construction: every buffer is
// fixed or sized from configuration.
//
// Driving contract for the server integrator:
//   * build one OuchUserStateDirectory for the day. The core's Sessions
//     policy calls user_state.on_events(events, core) once per batch that
//     concerns the user, with the user's events in batch order, and
//     directory.on_shutdown(end_of_day, core) at end of day.
//   * per accepted TCP connection, construct an OuchConnection with that
//     directory; connection.on_data(bytes, core) consumes every byte.
//   * call connection.on_timer(core) periodically (heartbeat, idle timeout,
//     paused replay) and connection.on_writable(core) after send space frees.
//   * when connection.close_requested(): close the socket (after flushing if
//     flush_before_close()) and then call connection.on_disconnect(core). Also
//     call on_disconnect(core) when the socket drops on its own, or for the
//     core's take_disconnect_requests. on_disconnect detaches the user and
//     calls core.unbind (cancel-on-disconnect).
//   * on_events never calls back into submit/bind/unbind, so it is safe to run
//     during dispatch; closes it causes are completed by the caller later.
//   * fault injection is read from the Context (consume_injected_reject,
//     consume_drop_out, has_pending_drop_out); `fault user X delay` belongs
//     to the server loop.

#include "engine/command.hpp"
#include "engine/event.hpp"
#include "gateway/byte_sink.hpp"
#include "gateway/ouch/ouch_mapping.hpp"
#include "gateway/sequenced_store.hpp"
#include "gateway/token_table.hpp"
#include "engine/instrument.hpp"
#include "protocol/ouch42.hpp"
#include "protocol/soupbintcp.hpp"
#include "server/config.hpp"
#include "server/core/policies.hpp"
#include "common/types.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace exsim::gateway::ouch {

// Longest sequenced OUCH message the server sends (Order Replaced) and its
// SoupBinTCP frame (2-byte length + 'S').
inline constexpr std::size_t kMaxOutboundOuchPayloadBytes = protocol::ouch42::OrderReplaced::wire_size;
inline constexpr std::size_t kMaxFramedOutboundBytes = kMaxOutboundOuchPayloadBytes + 3;

// SoupBinTCP recommends treating 15 s without any inbound packet as a lost link.
inline constexpr std::uint32_t kDefaultClientIdleTimeoutMs = 15'000;

struct OuchSessionSettings {
    std::array<char, 10> soup_session = protocol::wire::spaces<10>();
    Timestamp server_heartbeat_interval_ns = 1'000'000'000;
    // No inbound packet (data, heartbeat or login) for this long closes the
    // connection. Before login it is also the login deadline.
    Timestamp client_idle_timeout_ns = Timestamp{kDefaultClientIdleTimeoutMs} * 1'000'000;

    [[nodiscard]] static OuchSessionSettings from(
        const server::ExchangeSection& exchange,
        std::uint32_t client_idle_timeout_ms = kDefaultClientIdleTimeoutMs) noexcept {
        OuchSessionSettings settings;
        const std::string_view session =
            std::string_view(exchange.soup_session).substr(0, settings.soup_session.size());
        protocol::wire::alpha(settings.soup_session.data(), settings.soup_session.size(), session);
        settings.server_heartbeat_interval_ns = Timestamp{exchange.heartbeat_interval_ms} * 1'000'000;
        settings.client_idle_timeout_ns = Timestamp{client_idle_timeout_ms} * 1'000'000;
        return settings;
    }
};

struct OuchUserStateLimits {
    std::uint32_t max_tokens = server::EngineSection{}.max_tokens_per_session;  // day-used tokens (O + replacements)
    std::size_t store_messages = server::EngineSection{}.store_messages_per_session;  // sequenced messages kept
    std::size_t store_bytes = server::EngineSection{}.store_bytes_per_session;        // their framed bytes
    // Once the store has this many or fewer free messages (or bytes for that
    // many maximal messages), a new O/U/M request stores nothing (not even a
    // J): the connection is closed instead (StoreReserveReached), keeping
    // room for reports on orders that are already live.
    std::size_t store_reserve_messages = 1024;

    [[nodiscard]] static OuchUserStateLimits from(const server::EngineSection& engine) noexcept {
        OuchUserStateLimits limits;
        limits.max_tokens = engine.max_tokens_per_session;
        limits.store_messages = engine.store_messages_per_session;
        limits.store_bytes = engine.store_bytes_per_session;
        limits.store_reserve_messages = std::min<std::size_t>(1024, engine.store_messages_per_session / 16);
        return limits;
    }
};

struct OuchUserStatistics {
    std::uint64_t duplicate_tokens_ignored = 0;        // O/U with a token already used today
    std::uint64_t unknown_token_requests_ignored = 0;  // U/X naming a token never used today
    std::uint64_t malformed_messages_ignored = 0;      // unknown OUCH type or wrong length
    std::uint64_t gateway_rejects = 0;                 // J sent without reaching the engine
    std::uint64_t messages_lost_to_store_overflow = 0;
    std::uint64_t outbound_encode_failures = 0;
    std::uint64_t outbound_messages_dropped_by_fault = 0;
    std::uint64_t requests_refused_at_store_reserve = 0;  // O/U/M that closed the connection instead
};

enum class OuchConnectionState : std::uint8_t { AwaitingLogin, LoggedIn, Closed };

enum class OuchCloseReason : std::uint8_t {
    None,
    LoginRejected,        // Login Rejected sent
    ProtocolViolation,    // bad framing, unknown packet type, data before login, second login
    ClientLogout,         // Logout Request received
    ClientIdleTimeout,    // nothing received for client_idle_timeout_ns
    SendBufferFull,       // sink.prepare failed for live output
    StoreFull,            // the user's sequenced store overflowed; End of Session sent
    StoreReserveReached,  // O/U/M received with the store at its reserve; nothing stored, reconnect allowed
    FaultDropOut,         // `fault user X drop_out n`: n messages stored, not written, then the link is cut
    EndOfSession,         // end_session(): End of Session sent
    TransportClosed,      // on_disconnect() without an earlier close request
};

template <gateway::ByteSink ByteSink>
class OuchUserState;
template <gateway::ByteSink ByteSink>
class OuchConnection;

// What the server must provide: logins, bindings, instruments, submit, the
// clock, the uncross phase of the batch being dispatched and the per-user
// fault queries. server::ExchangeCore satisfies it. unbind triggers
// cancel-on-disconnect when the user is configured for it; submit stamps the
// command, runs the engine and dispatches the batch to the sessions before
// returning, and returns a server::SubmitResult.
template <class Context>
concept OuchServerContext =
    requires(Context& context, const Context& const_context, std::string_view text, SessionId session,
             server::ConnectionId connection, const Symbol& symbol, InstrumentId instrument, const Command& command) {
        { context.authenticate(text, text, server::Protocol::Ouch42) } -> std::same_as<const server::UserConfig*>;
        { context.bind(session, connection) } -> std::same_as<bool>;
        context.unbind(session, connection);
        { const_context.find_instrument(symbol) } -> std::same_as<InstrumentId>;
        { const_context.instrument(instrument) } -> std::same_as<const InstrumentConfig*>;
        { context.submit(command) } -> std::same_as<server::SubmitResult>;
        { const_context.now() } -> std::same_as<Timestamp>;
        { const_context.phase_left_by_batch(instrument) } -> std::same_as<Phase>;
        { context.consume_injected_reject(session) } -> std::same_as<bool>;
        { context.consume_drop_out(session) } -> std::same_as<bool>;
        { const_context.has_pending_drop_out(session) } -> std::same_as<bool>;
    };

// No gateway reject: the command reached the engine (or will, if deferred).
inline constexpr char kCommandApplied = '\0';

// Submits `command`. Returns the OUCH reject reason for a command the Context
// reports it did not apply (outside the trading day: 'C'), else kCommandApplied.
template <class Context>
[[nodiscard]] char submit_to_engine(Context& context, const Command& command) noexcept {
    switch (context.submit(command)) {
        case server::SubmitResult::Applied:
        case server::SubmitResult::Deferred: return kCommandApplied;
        case server::SubmitResult::OutsideTradingDay:
        case server::SubmitResult::ExchangeShutDown: return kOuchRejectExchangeClosed;
        case server::SubmitResult::OutputCapacityExceeded:
        case server::SubmitResult::DeferredQueueFull: return kOuchRejectOther;
    }
    return kOuchRejectOther;
}

// ============================================================================
// OuchUserState
// ============================================================================

template <gateway::ByteSink ByteSink>
class OuchUserState {
public:
    OuchUserState(const server::UserConfig& user, const OuchUserStateLimits& limits)
        : session_id_(user.session_id),
          owner_(user.owner),
          self_trade_prevention_(user.stp),
          tokens_(limits.max_tokens),
          instructions_by_cl_ord_id_(std::make_unique<OuchOrderInstructions[]>(std::size_t{tokens_.max_tokens()} + 1)),
          store_(limits.store_messages, limits.store_bytes),
          store_reserve_messages_(limits.store_reserve_messages) {}

    ~OuchUserState() { assert(attached_connection_ == nullptr); }

    // Connections keep a pointer to their user state: construct in place.
    OuchUserState(const OuchUserState&) = delete;
    OuchUserState& operator=(const OuchUserState&) = delete;
    OuchUserState(OuchUserState&&) = delete;
    OuchUserState& operator=(OuchUserState&&) = delete;

    [[nodiscard]] SessionId session_id() const noexcept { return session_id_; }

    // Encodes the events of this session, in the order given (batch order),
    // stores each message and writes it to the attached connection. Events
    // of other sessions are skipped, so `events` may be a whole batch or
    // only this session's events. Safe during dispatch: never calls back
    // into the context except the read-only instrument(), now() and
    // phase_left_by_batch().
    template <class Events, class Context>
        requires OuchServerContext<Context>
    void on_events(const Events& events, Context& context) noexcept {
        for (const Event& event : events) {
            switch (event.type) {
                case EventType::Accepted:
                    if (event.accepted.session == session_id_) append_order_accepted(event, context);
                    break;
                case EventType::Rejected:
                    if (event.rejected.session == session_id_) append_engine_reject(event, context);
                    break;
                case EventType::Fill: {
                    const FillEvent& fill = event.fill;
                    if (fill.buy.session != session_id_ && fill.sell.session != session_id_) break;
                    // An uncross (aggressor None) reports which call phase it ended.
                    const Phase phase_being_left = fill.aggressor == Aggressor::None
                                                       ? context.phase_left_by_batch(event.instrument)
                                                       : Phase::Continuous;
                    // Buy leg before sell leg, also when both legs are ours.
                    if (fill.buy.session == session_id_) {
                        append_order_executed(
                            event, fill.buy, ouch_liquidity_flag(fill.aggressor, Side::BUY, phase_being_left), context);
                    }
                    if (fill.sell.session == session_id_) {
                        append_order_executed(event, fill.sell,
                                              ouch_liquidity_flag(fill.aggressor, Side::SELL, phase_being_left),
                                              context);
                    }
                    break;
                }
                case EventType::Canceled:
                    if (event.canceled.session == session_id_) append_order_canceled(event, context);
                    break;
                case EventType::Replaced:
                    if (event.replaced.session == session_id_) append_order_replaced(event, context);
                    break;
                case EventType::Rested:
                case EventType::MassCancelDone:
                case EventType::PhaseChanged:
                case EventType::AuctionSummary: break;
            }
        }
    }

    // System Event 'E' (end of day) stamped `end_of_day`. Call after the
    // closing SetPhase batch (OuchUserStateDirectory::on_shutdown does).
    template <class Context>
        requires OuchServerContext<Context>
    void append_end_of_day_event(Timestamp end_of_day, Context& context) noexcept {
        append_system_event(kOuchSystemEventEndOfDay, end_of_day, context);
    }

    // Start of a new trading day: forget tokens, messages and per-day
    // statistics. Instructions need no clearing: an entry is rewritten
    // whenever its token is interned, before it is read.
    // Precondition: no connection attached.
    void reset_for_new_trading_day() noexcept {
        assert(attached_connection_ == nullptr);
        tokens_.clear();
        store_.clear();
        statistics_ = {};
        start_of_day_sent_ = false;
        store_overflowed_ = false;
    }

    [[nodiscard]] const SequencedStore& store() const noexcept { return store_; }
    [[nodiscard]] const TokenTable& tokens() const noexcept { return tokens_; }
    [[nodiscard]] ClOrdId find_cl_ord_id(std::string_view token) const noexcept { return tokens_.find(token); }
    [[nodiscard]] bool has_attached_connection() const noexcept { return attached_connection_ != nullptr; }
    [[nodiscard]] const OuchConnection<ByteSink>* attached_connection() const noexcept { return attached_connection_; }
    [[nodiscard]] bool store_overflowed() const noexcept { return store_overflowed_; }
    [[nodiscard]] bool start_of_day_sent() const noexcept { return start_of_day_sent_; }
    [[nodiscard]] const OuchUserStatistics& statistics() const noexcept { return statistics_; }

private:
    friend class OuchConnection<ByteSink>;
    using OuchToken = std::array<char, 14>;

    // ---- Called by OuchConnection ------------------------------------------

    void attach_connection(OuchConnection<ByteSink>* connection) noexcept { attached_connection_ = connection; }
    void detach_connection(const OuchConnection<ByteSink>* connection) noexcept {
        if (attached_connection_ == connection) attached_connection_ = nullptr;
    }

    // One OUCH message from a SoupBinTCP Unsequenced Data packet.
    template <class Context>
    void handle_application_message(std::string_view payload, Context& context) noexcept {
        if (payload.empty()) [[unlikely]] {
            ++statistics_.malformed_messages_ignored;
            return;
        }
        switch (payload.front()) {
            case protocol::ouch42::EnterOrder::type: handle_enter_order(payload, context); return;
            case protocol::ouch42::ReplaceOrder::type: handle_replace_order(payload, context); return;
            case protocol::ouch42::CancelOrder::type: handle_cancel_order(payload, context); return;
            case protocol::ouch42::ModifyOrder::type: handle_modify_order(payload, context); return;
            default: ++statistics_.malformed_messages_ignored; return;
        }
    }

    // System Event 'S' once per trading day, right after the first login.
    template <class Context>
    void append_start_of_day_if_needed(Context& context) noexcept {
        if (start_of_day_sent_) return;
        start_of_day_sent_ = true;
        append_system_event(kOuchSystemEventStartOfDay, context.now(), context);
    }

    // Fault injection (`fault user X drop_out n`): true if the
    // next live sequenced message must stay stored but not be transmitted.
    template <class Context>
    [[nodiscard]] bool consume_outbound_drop(Context& context) noexcept {
        if (!context.consume_drop_out(session_id_)) [[likely]]
            return false;
        ++statistics_.outbound_messages_dropped_by_fault;
        return true;
    }

    // ---- Inbound mapping -----------------------------------------------------

    template <class Context>
    void handle_enter_order(std::string_view payload, Context& context) noexcept {
        protocol::ouch42::EnterOrder request;
        const protocol::Status decoded = protocol::ouch42::decode(payload.data(), payload.size(), request);
        // invalid_field is an unknown side; the codec has already copied the token.
        if (decoded != protocol::Status::ok && decoded != protocol::Status::invalid_field) {
            ++statistics_.malformed_messages_ignored;
            return;
        }
        if (refuse_request_at_store_reserve()) return;
        const TokenTable::InternResult reservation = tokens_.intern(token_view(request.token));
        if (reservation.status == TokenTable::InternStatus::Existing) {
            ++statistics_.duplicate_tokens_ignored;  // day-unique tokens: reuse is suppressed silently
            return;
        }
        if (reservation.status != TokenTable::InternStatus::Inserted) {
            reject_at_gateway(request.token, kOuchRejectOther, context);  // token table full
            return;
        }
        if (decoded == protocol::Status::invalid_field) {
            reject_at_gateway(request.token, kOuchRejectOther, context);
            return;
        }
        if (context.consume_injected_reject(session_id_)) {  // `fault user X reject n`
            reject_at_gateway(request.token, kOuchRejectOther, context);
            return;
        }
        const InstrumentId instrument = context.find_instrument(request.stock);
        if (instrument == 0) {
            reject_at_gateway(request.token, kOuchRejectInvalidStock, context);
            return;
        }
        if (!enter_order_instructions_supported(request)) {
            reject_at_gateway(request.token, kOuchRejectOther, context);
            return;
        }
        instructions_by_cl_ord_id_[reservation.id] = instructions_of(request);

        NewOrder order;
        order.instrument = instrument;
        order.session = session_id_;
        order.owner = owner_;
        order.cl_ord_id = reservation.id;
        order.side = engine_side(request.side);
        order.type = request.price == kOuchMarketOrderPrice ? OrdType::Market : OrdType::Limit;
        order.tif = engine_time_in_force(request.time_in_force);
        order.stp = self_trade_prevention_;
        order.post_only = display_is_post_only(request.display);
        order.price = request.price;
        order.qty = request.shares;
        const char not_applied_reason = submit_to_engine(context, Command{order});
        if (not_applied_reason != kCommandApplied) [[unlikely]] {
            reject_at_gateway(request.token, not_applied_reason, context);
        }
    }

    template <class Context>
    void handle_replace_order(std::string_view payload, Context& context) noexcept {
        protocol::ouch42::ReplaceOrder request;
        if (protocol::ouch42::decode(payload.data(), payload.size(), request) != protocol::Status::ok) {
            ++statistics_.malformed_messages_ignored;
            return;
        }
        if (refuse_request_at_store_reserve()) return;
        const ClOrdId existing_id = tokens_.find(token_view(request.existing_token));
        const TokenTable::InternResult reservation = tokens_.intern(token_view(request.replacement_token));
        if (reservation.status == TokenTable::InternStatus::Existing) {
            ++statistics_.duplicate_tokens_ignored;
            return;
        }
        if (reservation.status != TokenTable::InternStatus::Inserted) {
            reject_at_gateway(request.replacement_token, kOuchRejectOther, context);
            return;
        }
        if (existing_id == 0) {
            ++statistics_.unknown_token_requests_ignored;  // OUCH ignores replaces of unknown orders
            return;
        }
        const OuchOrderInstructions& existing = instructions_by_cl_ord_id_[existing_id];
        if (!replace_instructions_supported(existing, request)) {
            reject_at_gateway(request.replacement_token, kOuchRejectOther, context);
            return;
        }
        instructions_by_cl_ord_id_[reservation.id] = instructions_after_replace(existing, request);

        ReplaceOrder replace;
        replace.session = session_id_;
        replace.orig_cl_ord_id = existing_id;
        replace.cl_ord_id = reservation.id;
        replace.price = request.price;
        replace.qty = request.shares;  // OUCH shares = new total quantity
        replace.force_priority_loss = true;
        const char not_applied_reason = submit_to_engine(context, Command{replace});
        if (not_applied_reason != kCommandApplied) [[unlikely]] {
            reject_at_gateway(request.replacement_token, not_applied_reason, context);
        }
    }

    // X shares is the new intended total. The engine's reduce_to_total_qty has
    // exactly the OUCH semantics: 0 or shares <= cum cancels fully,
    // cum < shares < cum + leaves reduces keeping priority, larger is a no-op.
    template <class Context>
    void handle_cancel_order(std::string_view payload, Context& context) noexcept {
        protocol::ouch42::CancelOrder request;
        if (protocol::ouch42::decode(payload.data(), payload.size(), request) != protocol::Status::ok) {
            ++statistics_.malformed_messages_ignored;
            return;
        }
        const ClOrdId order_id = tokens_.find(token_view(request.token));
        if (order_id == 0) {
            ++statistics_.unknown_token_requests_ignored;
            return;
        }
        CancelOrder cancel;
        cancel.session = session_id_;
        cancel.orig_cl_ord_id = order_id;
        cancel.reduce_to_total_qty = request.shares;
        (void)submit_to_engine(context, Command{cancel});  // OUCH has no cancel reject: not applied is silent
    }

    template <class Context>
    void handle_modify_order(std::string_view payload, Context& context) noexcept {
        protocol::ouch42::ModifyOrder request;
        const protocol::Status decoded = protocol::ouch42::decode(payload.data(), payload.size(), request);
        if (decoded != protocol::Status::ok && decoded != protocol::Status::invalid_field) {
            ++statistics_.malformed_messages_ignored;
            return;
        }
        if (refuse_request_at_store_reserve()) return;
        reject_at_gateway(request.token, kOuchRejectOther, context);  // Modify is not supported
    }

    template <class Context>
    void reject_at_gateway(const OuchToken& token, char reason, Context& context) noexcept {
        ++statistics_.gateway_rejects;
        protocol::ouch42::OrderRejected message;
        message.timestamp = context.now();
        message.token = token;
        message.reason = reason;
        append_sequenced(message, context);
    }

    // Admission for every request that may store a gateway J or a new
    // order's reports (O, U, M), checked before anything is stored. At the
    // reserve the request stores nothing: the connection closes and the
    // client may reconnect and replay. The reserve stays for reports on live
    // orders (X, fills, cancel-on-disconnect, end of day).
    [[nodiscard]] bool refuse_request_at_store_reserve() noexcept {
        if (store_admits_new_requests()) [[likely]]
            return false;
        ++statistics_.requests_refused_at_store_reserve;
        if (attached_connection_ != nullptr) attached_connection_->close(OuchCloseReason::StoreReserveReached);
        return true;
    }

    [[nodiscard]] bool store_admits_new_requests() const noexcept {
        const std::size_t free_messages = store_.max_messages() - static_cast<std::size_t>(store_.last_seq());
        const std::size_t free_bytes = store_.max_bytes() - store_.bytes_used();
        return free_messages > store_reserve_messages_ &&
               free_bytes / kMaxFramedOutboundBytes > store_reserve_messages_;
    }

    // ---- Outbound mapping ---------------------------------------------------

    template <class Context>
    void append_order_accepted(const Event& event, Context& context) noexcept {
        const AcceptedEvent& accepted = event.accepted;
        const OuchOrderInstructions& instructions = instructions_for(accepted.cl_ord_id);
        protocol::ouch42::OrderAccepted message;
        message.timestamp = event.ts;
        copy_token(accepted.cl_ord_id, message.token);
        message.side = instructions.side;
        message.shares = accepted.qty;
        message.stock = symbol_of(event.instrument, context);
        message.price = accepted.price;
        message.time_in_force = instructions.time_in_force_seconds;
        message.firm = instructions.firm;
        message.display = instructions.display;
        message.order_reference = accepted.order_id;
        message.capacity = instructions.capacity;
        message.ise = instructions.intermarket_sweep;
        message.minimum_quantity = instructions.minimum_quantity;
        message.cross_type = instructions.cross_type;
        message.order_state = kOuchOrderStateLive;  // no atomic-dead acknowledgement compression
        message.bbo_weight = kOuchBboWeightUnspecified;
        append_sequenced(message, context);
    }

    template <class Context>
    void append_engine_reject(const Event& event, Context& context) noexcept {
        const RejectedEvent& rejected = event.rejected;
        if (rejected.cmd != CommandType::NewOrder && rejected.cmd != CommandType::Replace) return;  // Cancel: ignored
        if (rejected.cmd == CommandType::Replace && rejected.reason == RejectReason::UnknownOrder) return;
        protocol::ouch42::OrderRejected message;
        message.timestamp = event.ts;
        copy_token(rejected.cl_ord_id, message.token);  // request token (replacement token for U)
        message.reason = ouch_reject_reason(rejected.reason);
        append_sequenced(message, context);
    }

    template <class Context>
    void append_order_executed(const Event& event, const FillLeg& leg, char liquidity_flag, Context& context) noexcept {
        protocol::ouch42::OrderExecuted message;
        message.timestamp = event.ts;
        copy_token(leg.cl_ord_id, message.token);
        message.shares = event.fill.qty;
        message.price = event.fill.price;
        message.liquidity = liquidity_flag;
        message.match_number = event.fill.match_id;
        append_sequenced(message, context);
    }

    template <class Context>
    void append_order_canceled(const Event& event, Context& context) noexcept {
        const CanceledEvent& canceled = event.canceled;
        protocol::ouch42::OrderCanceled message;
        message.timestamp = event.ts;
        copy_token(canceled.cl_ord_id, message.token);
        message.shares = canceled.canceled_qty;  // decrement shares
        message.reason = ouch_cancel_reason(canceled.reason);
        append_sequenced(message, context);
    }

    template <class Context>
    void append_order_replaced(const Event& event, Context& context) noexcept {
        const ReplacedEvent& replaced = event.replaced;
        const OuchOrderInstructions& instructions = instructions_for(replaced.cl_ord_id);
        protocol::ouch42::OrderReplaced message;
        message.timestamp = event.ts;
        copy_token(replaced.cl_ord_id, message.replacement_token);
        message.side = instructions.side;
        message.shares = replaced.leaves_qty;  // post-amend leaves
        message.stock = symbol_of(event.instrument, context);
        message.price = replaced.price;
        message.time_in_force = instructions.time_in_force_seconds;
        message.firm = instructions.firm;
        message.display = instructions.display;
        message.order_reference = replaced.new_order_id;
        message.capacity = instructions.capacity;
        message.ise = instructions.intermarket_sweep;
        message.minimum_quantity = instructions.minimum_quantity;
        message.cross_type = instructions.cross_type;
        message.order_state = kOuchOrderStateLive;
        copy_token(replaced.orig_cl_ord_id, message.previous_token);
        message.bbo_weight = kOuchBboWeightUnspecified;
        append_sequenced(message, context);
    }

    template <class Context>
    void append_system_event(char system_event, Timestamp event_time, Context& context) noexcept {
        protocol::ouch42::SystemEvent message;
        message.timestamp = event_time;
        message.event = system_event;
        append_sequenced(message, context);
    }

    // Frames `message` as SoupBinTCP Sequenced Data, stores it (assigning its
    // sequence number) and hands it to the attached connection. A full store
    // ends the user's session for the day: the message is lost, so replay
    // could never be complete again (see OuchCloseReason::StoreFull).
    template <class Message, class Context>
    void append_sequenced(const Message& message, Context& context) noexcept {
        if (store_overflowed_) [[unlikely]] {
            ++statistics_.messages_lost_to_store_overflow;
            return;
        }
        char payload[kMaxOutboundOuchPayloadBytes];
        const std::size_t payload_size = protocol::ouch42::encode(message, payload, sizeof payload);
        char frame[kMaxFramedOutboundBytes];
        const std::size_t frame_size =
            payload_size == 0 ? 0
                              : protocol::soupbintcp::encode(
                                    protocol::soupbintcp::SequencedData{{payload, payload_size}}, frame, sizeof frame);
        if (frame_size == 0) [[unlikely]] {
            ++statistics_.outbound_encode_failures;
            return;
        }
        if (!store_.append({frame, frame_size})) [[unlikely]] {
            store_overflowed_ = true;
            ++statistics_.messages_lost_to_store_overflow;
            if (attached_connection_ != nullptr) attached_connection_->terminate_for_store_overflow(context.now());
            return;
        }
        if (attached_connection_ != nullptr) attached_connection_->deliver_stored_messages(context);
    }

    [[nodiscard]] const OuchOrderInstructions& instructions_for(ClOrdId cl_ord_id) const noexcept {
        static constexpr OuchOrderInstructions kUnknownOrderInstructions{};
        if (cl_ord_id == 0 || cl_ord_id > tokens_.max_tokens()) [[unlikely]]
            return kUnknownOrderInstructions;
        return instructions_by_cl_ord_id_[cl_ord_id];
    }

    void copy_token(ClOrdId cl_ord_id, OuchToken& destination) const noexcept {
        const std::string_view token = tokens_.token(cl_ord_id);
        destination.fill(' ');
        if (!token.empty()) std::memcpy(destination.data(), token.data(), std::min(token.size(), destination.size()));
    }

    [[nodiscard]] static std::string_view token_view(const OuchToken& token) noexcept {
        return {token.data(), token.size()};  // byte-exact, padding included
    }

    template <class Context>
    [[nodiscard]] static Symbol symbol_of(InstrumentId instrument, const Context& context) noexcept {
        const InstrumentConfig* config = context.instrument(instrument);
        return config != nullptr ? config->symbol : Symbol{};
    }

    SessionId session_id_;
    OwnerId owner_;
    StpMode self_trade_prevention_;
    TokenTable tokens_;
    std::unique_ptr<OuchOrderInstructions[]> instructions_by_cl_ord_id_;  // index = ClOrdId
    SequencedStore store_;
    std::size_t store_reserve_messages_;
    OuchConnection<ByteSink>* attached_connection_ = nullptr;
    bool start_of_day_sent_ = false;
    bool store_overflowed_ = false;
    OuchUserStatistics statistics_{};
};

// ============================================================================
// OuchUserStateDirectory
// ============================================================================

// The OUCH users' states for the trading day. The server's session table
// (GatewaySessions) routes dispatch to them; find() serves logins.
template <gateway::ByteSink ByteSink>
class OuchUserStateDirectory {
public:
    // One user state per configured OUCH user.
    OuchUserStateDirectory(const server::ServerConfig& config, const OuchUserStateLimits& limits) {
        for (const server::UserConfig& user : config.users) {
            if (user.protocol != server::Protocol::Ouch42 || user.session_id == 0) continue;
            user_states_.push_back(std::make_unique<OuchUserState<ByteSink>>(user, limits));
        }
    }

    explicit OuchUserStateDirectory(const server::ServerConfig& config)
        : OuchUserStateDirectory(config, OuchUserStateLimits::from(config.engine)) {}

    // User states are referenced by connections: keep the directory in place.
    OuchUserStateDirectory(const OuchUserStateDirectory&) = delete;
    OuchUserStateDirectory& operator=(const OuchUserStateDirectory&) = delete;
    OuchUserStateDirectory(OuchUserStateDirectory&&) = delete;
    OuchUserStateDirectory& operator=(OuchUserStateDirectory&&) = delete;

    // nullptr if `session` is not an OUCH user's. Linear in the OUCH users:
    // login path only.
    [[nodiscard]] OuchUserState<ByteSink>* find(SessionId session) const noexcept {
        for (const std::unique_ptr<OuchUserState<ByteSink>>& user_state : user_states_) {
            if (user_state->session_id() == session) return user_state.get();
        }
        return nullptr;
    }

    [[nodiscard]] std::span<const std::unique_ptr<OuchUserState<ByteSink>>> users() const noexcept {
        return user_states_;
    }

    // End of day, after the closing SetPhase batch: System Event 'E' for every
    // user (stored for those not logged in, so a later login recovers it).
    template <class Context>
        requires OuchServerContext<Context>
    void on_shutdown(Timestamp end_of_day, Context& context) noexcept {
        for (const std::unique_ptr<OuchUserState<ByteSink>>& user_state : user_states_) {
            user_state->append_end_of_day_event(end_of_day, context);
        }
    }

    // Precondition: no connection attached to any user state.
    void reset_for_new_trading_day() noexcept {
        for (const std::unique_ptr<OuchUserState<ByteSink>>& user_state : user_states_) {
            user_state->reset_for_new_trading_day();
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return user_states_.size(); }

private:
    std::vector<std::unique_ptr<OuchUserState<ByteSink>>> user_states_;
};

}  // namespace exsim::gateway::ouch

#endif
