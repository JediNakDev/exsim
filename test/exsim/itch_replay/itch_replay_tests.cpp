// End-to-end ITCH replay validation:
//   1. Generate a deterministic synthetic ITCH 5.0 stream.
//   2. Decode each message with protocol::itch50 through ItchReplayer::apply.
//   3. Drive both the matching engine (MatchingEngine::handle) and a
//      ReferenceBook from the same message sequence.
//   4. After every K messages diff the top-10 of both books; fail if they
//      ever disagree.
//   5. After the full stream, assert ordering / live-order invariants.
//
// This is the harness that backs the "book reconstruction validated
// against a reference oracle" claim. Real NASDAQ ITCH files can be
// dropped through tools/itch_replay (same dispatch) with the same
// invariant checks toggled on.

#include "test/framework/test.hpp"
#include "tools/itch_replay/diff.hpp"
#include "tools/itch_replay/itch_replayer.hpp"
#include "tools/itch_replay/reference_book.hpp"
#include "tools/itch_replay/synthetic.hpp"

#include <iostream>

TEST_CASE(itch_replay, replay_matches_reference) {
    constexpr std::size_t kMessages = 100'000;
    constexpr std::size_t kDiffEvery = 1;  // diff after every message

    exsim::itch_replay::SyntheticItchStream gen(/*seed=*/0xC0FFEE, kMessages, /*mid=*/10000);
    auto wire = gen.build();

    exsim::itch_replay::ReferenceBook ref;
    exsim::itch_replay::ItchReplayer replayer(exsim::itch_replay::max_live_orders_for(wire.size()), &ref);
    const exsim::MatchingEngine& engine = replayer.engine();

    std::size_t offset = 0;
    std::size_t parsed = 0;
    while (offset < wire.size()) {
        const std::size_t sz = exsim::itch_replay::book_message_size(wire[offset]);
        REQUIRE(sz > 0 && "unknown message type in synthetic stream");
        REQUIRE(offset + sz <= wire.size());

        const char type = wire[offset];
        bool ok = replayer.apply({&wire[offset], sz});
        REQUIRE(ok);
        offset += sz;
        ++parsed;

        if (parsed % kDiffEvery == 0) {
            auto d = exsim::itch_replay::diff_books(engine, replayer.instrument(), ref, /*depth=*/10);
            if (!d.ok) {
                FAIL("DIVERGENCE after " + std::to_string(parsed) + " msgs (type='" + type + "'): " + d.detail);
            }
        }
    }

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
