// End-to-end ITCH replay validation:
//   1. Generate a deterministic synthetic ITCH 5.0 stream.
//   2. Walk its frames with for_each_message and decode each message with
//      protocol::itch50 through ItchReplayer::apply.
//   3. Drive both the matching engine (MatchingEngine::handle) and a
//      ReferenceBook from the same message sequence.
//   4. After every K messages diff the top-10 of both books; fail if they
//      ever disagree.
//   5. After the full stream, assert ordering / live-order invariants.
//
// This is the harness that backs the "book reconstruction validated
// against a reference oracle" claim. A NASDAQ file goes through the same
// frame walker and dispatch in tools/itch_replay, one stock at a time.

#include "test/framework/test.hpp"
#include "tools/itch_replay/diff.hpp"
#include "tools/itch_replay/itch_replayer.hpp"
#include "tools/itch_replay/reference_book.hpp"
#include "tools/itch_replay/synthetic.hpp"

#include <protocol/itch50.hpp>

#include <iostream>
#include <span>
#include <vector>

namespace itch50 = exsim::protocol::itch50;

namespace {

// Appends one message after its 2-byte big-endian length, as NASDAQ files store it.
template <typename Message>
void append_framed(std::vector<char>& file, const Message& message) {
    char body[Message::wire_size];
    REQUIRE(itch50::encode(message, body, sizeof body) == sizeof body);
    file.push_back(static_cast<char>(sizeof body >> 8));
    file.push_back(static_cast<char>(sizeof body & 0xFF));
    file.insert(file.end(), body, body + sizeof body);
}

itch50::StockDirectory directory(std::uint16_t locate, const char* stock) {
    itch50::StockDirectory m;
    m.stock_locate = locate;
    m.stock = exsim::Symbol(stock);
    return m;
}

itch50::AddOrder add(std::uint16_t locate, std::uint64_t reference, exsim::Side side, exsim::Price price,
                     const char* stock) {
    itch50::AddOrder m;
    m.stock_locate = locate;
    m.order_reference = reference;
    m.side = side;
    m.shares = 100;
    m.price = price;
    m.stock = exsim::Symbol(stock);
    return m;
}

}  // namespace

TEST_CASE(itch_replay, replay_matches_reference) {
    constexpr std::size_t kMessages = 100'000;
    constexpr std::size_t kDiffEvery = 1;  // diff after every message

    exsim::itch_replay::SyntheticItchStream gen(/*seed=*/0xC0FFEE, kMessages, /*mid=*/10000);
    auto wire = gen.build();

    exsim::itch_replay::ReferenceBook ref;
    exsim::itch_replay::ItchReplayer replayer(exsim::itch_replay::max_live_orders_for(wire.size()), &ref);
    const exsim::MatchingEngine& engine = replayer.engine();

    std::size_t parsed = 0;
    const bool complete = exsim::itch_replay::for_each_message(wire, [&](std::span<const char> message) {
        REQUIRE(replayer.apply(message));
        ++parsed;
        if (parsed % kDiffEvery == 0) {
            auto d = exsim::itch_replay::diff_books(engine, replayer.instrument(), ref, /*depth=*/10);
            if (!d.ok) {
                FAIL("DIVERGENCE after " + std::to_string(parsed) + " msgs (type='" + message[0] + "'): " + d.detail);
            }
        }
    });
    REQUIRE(complete);
    REQUIRE(parsed == kMessages);

    auto final_diff = exsim::itch_replay::diff_books(engine, replayer.instrument(), ref, /*depth=*/20);
    if (!final_diff.ok) {
        FAIL("FINAL DIVERGENCE: " + final_diff.detail);
    }

    auto const& s = replayer.stats();
    REQUIRE(s.total() == parsed && "every parsed message must be dispatched");
    REQUIRE(s.dropped_cross == 0 && "synthetic stream is non-marketable by construction");
    REQUIRE(s.rejected == 0 && "every synthetic message fits the replay instrument");
    REQUIRE(engine.live_order_count() == ref.live_orders());

    auto bid = engine.best_bid(replayer.instrument());
    auto ask = engine.best_ask(replayer.instrument());
    if (bid && ask) {
        REQUIRE(*bid < *ask && "engine reports crossed book");
    }

    std::cout << "[itch] parsed=" << parsed << " adds=" << s.adds << " execs=" << s.executes << " cancels=" << s.cancels
              << " deletes=" << s.deletes << " replaces=" << s.replaces << " unknown_ref=" << s.unknown_ref << " ";
}

// A NASDAQ file interleaves stocks and holds messages that do not change a
// book; only the chosen stock's book messages reach the engine.
TEST_CASE(itch_replay, file_replays_one_stock_with_executions) {
    std::vector<char> file;
    append_framed(file, itch50::SystemEvent{});
    append_framed(file, directory(1, "AAPL"));
    append_framed(file, directory(2, "MSFT"));
    append_framed(file, add(1, 1, exsim::Side::BUY, 10000, "AAPL"));
    itch50::OrderExecutedWithPrice executed;
    executed.stock_locate = 1;
    executed.order_reference = 1;
    executed.shares = 100;
    executed.price = 10000;
    append_framed(file, executed);
    append_framed(file, add(1, 2, exsim::Side::BUY, 9900, "AAPL"));
    append_framed(file, add(2, 3, exsim::Side::SELL, 9000, "MSFT"));

    exsim::itch_replay::ItchReplayer replayer(exsim::itch_replay::max_live_orders_for(file.size()), nullptr,
                                              exsim::Symbol("AAPL"));
    std::size_t applied = 0;
    REQUIRE(exsim::itch_replay::for_each_message(file, [&](std::span<const char> message) {
        if (replayer.apply(message)) ++applied;
    }));
    CHECK_EQ(applied, 3u);
    CHECK_EQ(replayer.stats().adds, 2u);
    CHECK_EQ(replayer.stats().executes, 1u);
    CHECK_EQ(replayer.stats().dropped_cross, 0u);
    CHECK_EQ(replayer.engine().live_order_count(), 1u);
    CHECK(replayer.engine().best_bid(replayer.instrument()) == exsim::Price{9900});
}

TEST_CASE(itch_replay, file_ending_inside_a_message_is_an_error) {
    std::vector<char> file;
    append_framed(file, add(1, 1, exsim::Side::BUY, 10000, "AAPL"));
    const std::size_t first_message_end = file.size();
    append_framed(file, add(1, 2, exsim::Side::BUY, 9900, "AAPL"));
    std::size_t messages = 0;
    const auto count = [&](std::span<const char>) { ++messages; };
    // Cut inside the second length prefix or the second body.
    for (std::size_t size = first_message_end + 1; size < file.size(); ++size) {
        messages = 0;
        CHECK(!exsim::itch_replay::for_each_message(std::span<const char>(file).first(size), count));
        CHECK_EQ(messages, 1u);
    }
    messages = 0;
    CHECK(exsim::itch_replay::for_each_message(file, count));
    CHECK_EQ(messages, 2u);
}
