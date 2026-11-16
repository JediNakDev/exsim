#ifndef EXSIM_BENCH_SCENARIO_HPP
#define EXSIM_BENCH_SCENARIO_HPP

// One MatchingEngine with one instrument, driven only through its command
// interface the way ExchangeCore drives it: build a Command, clear the
// EventBuffer, call handle(). The helpers here build commands for the cases.

#include <engine/command.hpp>
#include <engine/event.hpp>
#include <engine/instrument.hpp>
#include <engine/matching_engine.hpp>

#include <cstddef>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <vector>

namespace bench {

inline constexpr exsim::SessionId kSession = 1;
inline constexpr std::size_t kMaxOrders = std::size_t{1} << 17;
// Matches the default InstrumentConfig: $0.01 tick and $100.00 reference price.
inline constexpr exsim::Price kTickSize = 100;
inline constexpr exsim::Price kMidPrice = 1'000'000;
inline constexpr int kLevelsPerSide = 500;

// Price `level` ticks away from the middle on the passive side of `side`.
inline exsim::Price price_at(exsim::Side side, int level) {
    const exsim::Price distance = static_cast<exsim::Price>(level) * kTickSize;
    return side == exsim::Side::BUY ? kMidPrice - distance : kMidPrice + distance;
}

struct RestingOrder {
    exsim::ClOrdId cl_ord_id;
    exsim::Side side;
};

class Scenario {
public:
    Scenario()
        : engine_(exsim::EngineConfig{.max_instruments = 1, .max_orders = kMaxOrders}),
          events_(exsim::required_event_capacity({.max_instruments = 1, .max_orders = kMaxOrders})),
          rng_(42) {
        exsim::InstrumentConfig config;
        config.symbol = exsim::Symbol("BENCH");
        instrument_ = engine_.add_instrument(config);
        if (instrument_ == 0) throw std::runtime_error("benchmark instrument rejected");
    }

    exsim::MatchingEngine& engine() { return engine_; }
    exsim::EventBuffer& events() { return events_; }
    std::mt19937_64& rng() { return rng_; }

    exsim::ClOrdId next_cl_ord_id() { return next_cl_ord_id_++; }

    exsim::Command new_order(exsim::ClOrdId cl_ord_id, exsim::Side side, exsim::Price price,
                             exsim::Quantity qty) const {
        exsim::NewOrder order;
        order.instrument = instrument_;
        order.session = kSession;
        order.cl_ord_id = cl_ord_id;
        order.side = side;
        order.tif = exsim::TimeInForce::GTC;
        order.price = price;
        order.qty = qty;
        return exsim::Command(order);
    }

    static exsim::Command cancel(exsim::ClOrdId cl_ord_id) {
        exsim::CancelOrder cancel;
        cancel.session = kSession;
        cancel.orig_cl_ord_id = cl_ord_id;
        return exsim::Command(cancel);
    }

    // New token, new price, losing time priority, as OUCH Replace does.
    static exsim::Command replace(exsim::ClOrdId old_cl_ord_id, exsim::ClOrdId new_cl_ord_id, exsim::Price price,
                                  exsim::Quantity qty) {
        exsim::ReplaceOrder replace;
        replace.session = kSession;
        replace.orig_cl_ord_id = old_cl_ord_id;
        replace.cl_ord_id = new_cl_ord_id;
        replace.price = price;
        replace.qty = qty;
        replace.force_priority_loss = true;
        return exsim::Command(replace);
    }

    // Untimed setup path. Every command must be applied.
    void apply(const exsim::Command& command) {
        events_.clear();
        if (engine_.handle(command, events_) != exsim::HandleResult::Applied) {
            throw std::runtime_error("benchmark setup command not applied");
        }
        for (const auto& event : events_) {
            if (event.type == exsim::EventType::Rejected) throw std::runtime_error("benchmark setup command rejected");
        }
    }

    // Rests `count` non-crossing orders of random side, level and quantity.
    std::vector<RestingOrder> rest_random_orders(std::size_t count) {
        std::vector<RestingOrder> resting;
        resting.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const RestingOrder order{next_cl_ord_id(), random_side()};
            apply(new_order(order.cl_ord_id, order.side, random_price(order.side), random_quantity()));
            resting.push_back(order);
        }
        return resting;
    }

    exsim::Side random_side() { return rng_() % 2 == 0 ? exsim::Side::BUY : exsim::Side::SELL; }
    exsim::Price random_price(exsim::Side side) {
        return price_at(side, static_cast<int>(rng_() % kLevelsPerSide) + 1);
    }
    exsim::Quantity random_quantity() { return static_cast<exsim::Quantity>(rng_() % 1000) + 1; }

private:
    exsim::MatchingEngine engine_;
    exsim::EventBuffer events_;
    std::mt19937_64 rng_;
    exsim::InstrumentId instrument_ = 0;
    exsim::ClOrdId next_cl_ord_id_ = 1;
};

}  // namespace bench

#endif
