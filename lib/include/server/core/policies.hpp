#ifndef EXSIM_SERVER_CORE_POLICIES_HPP
#define EXSIM_SERVER_CORE_POLICIES_HPP

// Compile-time contracts between ExchangeCore and the parts the server
// integrator plugs into it. Templates and
// concepts only: no virtual dispatch on the hot path.
//
// Required:
//   Clock       c.day_ns()        ns since local midnight (monotonic clock
//                                 recommended; the core enforces strict
//                                 increase of command timestamps itself)
//               c.realtime_ns()   ns since the Unix epoch (FIX SendingTime)
//   MarketData  md.on_events(std::span<const Event>)   one call per batch
//               md.flush()                            send pending output: after
//                                 each batch and each poll iteration.
//                                 ITCH: the open MoldUDP64 packet. MDP:
//                                 queued replay TCP bytes (its UDP packets
//                                 are sent as they are built).
//   Sessions    s.on_events(SessionId, const SessionEvents&, Core&)
//                                 once per distinct session of a batch,
//                                 first-appearance order, with that
//                                 session's events in batch order
//
// Optional (detected with requires-expressions):
//   md.on_timer(Timestamp, const Core&) or md.on_timer(Timestamp)
//                                 heartbeat / NOII, called from core.timer()
//   md.next_sequence()            MoldUDP64 next sequence, shown by `status`
//   md.on_shutdown(Timestamp)     ITCH end of system hours / end of session
//   s.on_shutdown(Timestamp, Core&)  OUCH/FIX end-of-day messages
// The core ignores what the market data hooks return; a feed that stops
// reports itself through md.state() and md.stop_reason() (admin `status`).
// The admin hooks (packet faults, publisher state) are declared with
// CoreAdmin, so the core does not depend on the md/ layer.

#include "engine/event.hpp"
#include "engine/types.hpp"
#include "server/core/session_dispatch_set.hpp"

#include <concepts>
#include <cstdint>
#include <span>
#include <string_view>

namespace exsim::server {

// Opaque to the core: stored per bound session and handed back to the
// server loop. The integrator packs net::ConnectionId {slot, generation}
// into it, for example (std::uint64_t{slot} << 32) | generation.
using ConnectionId = std::uint64_t;

enum class SubmitResult : std::uint8_t {
    Applied = 0,                 // handled by the engine (semantic rejections included), journaled, dispatched
    Deferred = 1,                // submitted during dispatch: queued, applied once the current batch is dispatched
    OutsideTradingDay = 2,       // stamped timestamp outside [trading_day_start, trading_day_end): not applied
    OutputCapacityExceeded = 3,  // EventBuffer smaller than the engine's bound (test-only sizing): not applied
    DeferredQueueFull = 4,       // re-entrant submit beyond the deferred queue: not applied
    ExchangeShutDown = 5,        // the exchange has shut down for the day: not applied, journaled or dispatched
};

[[nodiscard]] constexpr std::string_view to_string(SubmitResult r) noexcept {
    switch (r) {
        case SubmitResult::Applied: return "applied";
        case SubmitResult::Deferred: return "deferred";
        case SubmitResult::OutsideTradingDay: return "outside trading day";
        case SubmitResult::OutputCapacityExceeded: return "event buffer too small";
        case SubmitResult::DeferredQueueFull: return "deferred queue full";
        case SubmitResult::ExchangeShutDown: return "exchange shut down";
    }
    return "?";
}

template <typename Clock>
concept CoreClock = requires(const Clock& clock) {
    { clock.day_ns() } -> std::convertible_to<Timestamp>;
    { clock.realtime_ns() } -> std::convertible_to<std::uint64_t>;
};

template <typename MarketData>
concept CoreMarketData = requires(MarketData& market_data, std::span<const Event> batch) {
    market_data.on_events(batch);
    market_data.flush();
};

template <typename Sessions, typename Core>
concept CoreSessions = requires(Sessions& sessions, SessionId session, const SessionEvents& events, Core& core) {
    sessions.on_events(session, events, core);
};

// ---- optional hooks ------------------------------------------------------

template <typename MarketData, typename Core>
concept MarketDataTimerWithCore =
    requires(MarketData& market_data, Timestamp now, const Core& core) { market_data.on_timer(now, core); };

template <typename MarketData>
concept MarketDataTimer = requires(MarketData& market_data, Timestamp now) { market_data.on_timer(now); };

template <typename MarketData>
concept MarketDataSequence = requires(const MarketData& market_data) {
    { market_data.next_sequence() } -> std::convertible_to<std::uint64_t>;
};

template <typename MarketData>
concept MarketDataShutdown = requires(MarketData& market_data, Timestamp now) { market_data.on_shutdown(now); };

template <typename Sessions, typename Core>
concept SessionsShutdown = requires(Sessions& sessions, Timestamp now, Core& core) { sessions.on_shutdown(now, core); };

}  // namespace exsim::server

#endif
