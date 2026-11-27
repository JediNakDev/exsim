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
// frame walker and dispatch in benchmark/itch_replay, one stock at a time.

#include "test/framework/test.hpp"
#include "benchmark/itch_replay/diff.hpp"
#include "benchmark/itch_replay/itch_replayer.hpp"
#include "benchmark/itch_replay/reference_book.hpp"
#include "benchmark/itch_replay/synthetic.hpp"

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
// book; only the chosen stock's book messages reach the engine, under the
// locate of the first R message that names it.
TEST_CASE(itch_replay, file_replays_one_stock_with_executions) {
    std::vector<char> file;
    append_framed(file, itch50::SystemEvent{});
    append_framed(file, directory(1, "AAPL"));
    append_framed(file, directory(2, "MSFT"));
    append_framed(file, directory(3, "AAPL"));
    append_framed(file, add(1, 1, exsim::Side::BUY, 10000, "AAPL"));
    itch50::OrderExecutedWithPrice executed;  // non-printable, away from the display price
    executed.stock_locate = 1;
    executed.order_reference = 1;
    executed.shares = 100;
    executed.printable = 'N';
    executed.price = 9999;
    append_framed(file, executed);
    append_framed(file, add(1, 2, exsim::Side::BUY, 9900, "AAPL"));
    append_framed(file, add(2, 3, exsim::Side::SELL, 9000, "MSFT"));

    exsim::itch_replay::ItchReplayer replayer(exsim::itch_replay::max_live_orders_for(file.size()), nullptr,
                                              exsim::Symbol("AAPL"));
    std::size_t applied = 0;
    REQUIRE(exsim::itch_replay::for_each_message(file, [&](std::span<const char> message) {
        if (replayer.apply(message)) ++applied;
    }));
    CHECK(replayer.stock_found());
    CHECK_EQ(applied, 3u);
    CHECK_EQ(replayer.stats().adds, 2u);
    CHECK_EQ(replayer.stats().executes, 1u);
    CHECK_EQ(replayer.stats().dropped_cross, 0u);
    CHECK_EQ(replayer.engine().live_order_count(), 1u);
    CHECK(replayer.engine().best_bid(replayer.instrument()) == exsim::Price{9900});

    exsim::itch_replay::ItchReplayer missing(exsim::itch_replay::max_live_orders_for(file.size()), nullptr,
                                             exsim::Symbol("APPL"));
    REQUIRE(exsim::itch_replay::for_each_message(
        file, [&](std::span<const char> message) { CHECK(!missing.apply(message)); }));
    CHECK(!missing.stock_found());
}

// 'U' retires the old reference, so a replacement the engine refuses (here a
// price above the $200 band) must not leave the old order in the book.
TEST_CASE(itch_replay, replace_outside_the_band_retires_the_old_order) {
    std::vector<char> file;
    append_framed(file, add(1, 1, exsim::Side::BUY, 1'990'000, "AAPL"));
    itch50::OrderReplace replace;
    replace.stock_locate = 1;
    replace.original_reference = 1;
    replace.new_reference = 2;
    replace.shares = 100;
    replace.price = 2'010'000;
    append_framed(file, replace);

    exsim::itch_replay::ReferenceBook ref;
    exsim::itch_replay::ItchReplayer replayer(exsim::itch_replay::max_live_orders_for(file.size()), &ref);
    REQUIRE(exsim::itch_replay::for_each_message(
        file, [&](std::span<const char> message) { REQUIRE(replayer.apply(message)); }));
    CHECK_EQ(replayer.stats().rejected, 1u);
    CHECK_EQ(replayer.engine().live_order_count(), 0u);
    CHECK_EQ(ref.live_orders(), 0u);
    CHECK(exsim::itch_replay::diff_books(replayer.engine(), replayer.instrument(), ref, /*depth=*/10).ok);
}

TEST_CASE(itch_replay, file_with_a_cut_or_empty_message_is_an_error) {
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

    // A zero length cannot hold an ITCH message type.
    std::vector<char> empty_frame(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(first_message_end));
    empty_frame.insert(empty_frame.end(), {0, 0});
    messages = 0;
    CHECK(!exsim::itch_replay::for_each_message(empty_frame, count));
    CHECK_EQ(messages, 1u);
}
