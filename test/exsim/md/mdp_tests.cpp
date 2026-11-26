#include "md/mdp_publisher.hpp"
#include "md/mdp_replay_session.hpp"
#include "test/exsim/support/command_gen.hpp"
#include "test/exsim/support/format.hpp"
#include "test/framework/test.hpp"
#include "test/framework/alloc_counter.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <random>

namespace {
using namespace exsim;
namespace cme = protocol::cme;
struct Packet {
    std::vector<char> bytes;
    md::MdpStream stream;
    bool operator==(const Packet&) const = default;
};
struct Capture {
    std::vector<Packet>* packets;
    void operator()(std::span<const char> bytes, md::MdpStream stream) {
        packets->push_back({{bytes.begin(), bytes.end()}, stream});
    }
};
server::ServerConfig mdp_config(std::size_t instrument_count = 1) {
    server::ServerConfig config;
    config.engine.engine = {2, 32};
    config.market_data.protocol = server::MarketDataProtocol::Mdp3;
    config.market_data.replay_buffer_packets = 64;
    config.market_data.replay_username = "CME";
    config.market_data.replay_password = "secret";
    config.market_data.snapshot_interval_ms = 1;
    for (std::size_t index = 0; index < instrument_count; ++index) {
        InstrumentConfig instrument;
        instrument.symbol = Symbol(index == 0 ? "ES" : "NQ");
        instrument.cme_security_id = 800001 + static_cast<std::int32_t>(index);
        instrument.tick_size = 100;
        instrument.min_price = 100;
        instrument.max_price = 100000;
        instrument.reference_price = 10000;
        instrument.max_order_qty = 1000000000;
        if (index == 0) {
            const std::string_view name = "LONG-CME-SYMBOL-1234";
            std::copy(name.begin(), name.end(), instrument.cme_symbol.begin());
        }
        config.instruments.push_back(instrument);
    }
    return config;
}

// Independent MBP consumer. Every snapshot entry and incremental entry is dispatched by SecurityID to that
// instrument's own book, which applies New/Change/Delete rank semantics and checks its own RptSeq continuity.
// Event flags are channel-wide: one batch ends with exactly one EndOfEvent, on its last message.
struct Book {
    struct Level {
        std::int64_t price;
        std::int32_t qty;
        std::int32_t orders;
    };
    std::vector<Level> bids, asks;
    std::uint32_t report = 0;
};
struct Consumer {
    std::size_t depth = 10;  // the publisher's book_depth
    std::map<std::int32_t, Book> books;
    unsigned end_events = 0;
    bool batch_ended = false;
    void packet(const Packet& packet) {
        cme::MdpPacketView view;
        REQUIRE_EQ(cme::decode_mdp_packet(packet.bytes, view), protocol::Status::ok);
        cme::MessageView message;
        REQUIRE_EQ(cme::decode_mdp_message(view.messages, message), protocol::Status::ok);
        if (packet.stream == md::MdpStream::Snapshot) {
            REQUIRE_EQ(message.header.template_id, 52);
            auto& book = books[cme::load_le<std::int32_t>(message.body.data() + 8)];
            book.report = cme::load_le<std::uint32_t>(message.body.data() + 12);
            book.bids.clear();
            book.asks.clear();
            cme::GroupView entries;
            REQUIRE_EQ(cme::decode_group(message.trailing(), 22, entries), protocol::Status::ok);
            for (unsigned i = 0; i < entries.count; ++i) {
                const auto entry = entries.entry(i);
                auto& side = entry[21] == '0' ? book.bids : book.asks;
                side.push_back({cme::load_le<std::int64_t>(entry.data()), cme::load_le<std::int32_t>(entry.data() + 8),
                                cme::load_le<std::int32_t>(entry.data() + 12)});
            }
        } else if (packet.stream == md::MdpStream::Incremental &&
                   (message.header.template_id == 46 || message.header.template_id == 48)) {
            CHECK(!batch_ended);
            if (static_cast<unsigned char>(message.body[8]) & 0x80) {
                ++end_events;
                batch_ended = true;
            }
            cme::GroupView entries;
            REQUIRE_EQ(cme::decode_group(message.trailing(), 32, entries), protocol::Status::ok);
            for (unsigned i = 0; i < entries.count; ++i) {
                const auto entry = entries.entry(i);
                auto& book = books[cme::load_le<std::int32_t>(entry.data() + 12)];
                CHECK_EQ(cme::load_le<std::uint32_t>(entry.data() + 16), ++book.report);
                if (message.header.template_id == 48) continue;
                auto& side = entry[26] == '0' ? book.bids : book.asks;
                const auto rank = static_cast<std::size_t>(static_cast<unsigned char>(entry[24]) - 1);
                const auto action = static_cast<unsigned char>(entry[25]);
                Book::Level level{cme::load_le<std::int64_t>(entry.data()),
                                  cme::load_le<std::int32_t>(entry.data() + 8),
                                  cme::load_le<std::int32_t>(entry.data() + 20)};
                if (action == 0) {
                    REQUIRE(rank <= side.size());
                    side.insert(side.begin() + rank, level);
                    if (side.size() > depth) side.resize(depth);
                } else {
                    REQUIRE(rank < side.size());
                    if (action == 1)
                        side[rank] = level;
                    else {
                        REQUIRE_EQ(action, 2);
                        side.erase(side.begin() + rank);
                    }
                }
                for (std::size_t level_index = 1; level_index < side.size(); ++level_index)
                    CHECK(entry[26] == '0' ? side[level_index - 1].price > side[level_index].price
                                           : side[level_index - 1].price < side[level_index].price);
            }
        }
    }
    // Instrument n of the config is engine instrument n + 1.
    void compare(const MatchingEngine& engine, const std::vector<InstrumentConfig>& instruments) {
        for (std::size_t instrument = 0; instrument < instruments.size(); ++instrument) {
            const auto& book = books[instruments[instrument].cme_security_id];
            for (const auto side : {Side::BUY, Side::SELL}) {
                std::array<LevelInfo, 10> levels{};
                const auto count = engine.depth(static_cast<InstrumentId>(instrument + 1), side, levels.data(), depth);
                const auto& actual = side == Side::BUY ? book.bids : book.asks;
                REQUIRE_EQ(actual.size(), count);
                for (std::size_t i = 0; i < count; ++i) {
                    CHECK_EQ(actual[i].price, levels[i].price * 100000);
                    CHECK_EQ(static_cast<std::uint64_t>(actual[i].qty), levels[i].qty);
                    CHECK_EQ(static_cast<std::uint32_t>(actual[i].orders), levels[i].order_count);
                }
            }
        }
    }
};
struct Exchange {
    server::ServerConfig config;
    MatchingEngine engine;
    EventBuffer events;
    std::vector<Packet> packets;
    md::MdpPublisher<Capture> publisher;
    Timestamp now = 1000;
    explicit Exchange(std::size_t instrument_count = 1)
        : config(mdp_config(instrument_count)),
          engine(config.engine.engine),
          events(required_event_capacity(config.engine.engine)),
          publisher(config, Capture{&packets}) {
        for (std::size_t instrument = 0; instrument < instrument_count; ++instrument)
            REQUIRE_EQ(engine.add_instrument(config.instruments[instrument]), instrument + 1);
        REQUIRE_EQ(publisher.start(now, 1700000000000000000ull + now, engine), md::PublishStatus::Ok);
    }
    void submit(Command command, Consumer* consumer = nullptr) {
        events.clear();
        packets.clear();
        ++now;
        command.timestamp = now;
        REQUIRE_EQ(engine.handle(command, events), HandleResult::Applied);
        REQUIRE_EQ(publisher.on_events({events.begin(), events.size()}), md::PublishStatus::Ok);
        if (consumer) {
            consumer->batch_ended = false;
            for (const auto& packet : packets) consumer->packet(packet);
            if (!packets.empty()) CHECK(consumer->batch_ended);
            consumer->compare(engine, config.instruments);
        }
    }
    void order(ClOrdId id, Side side, Price price, Quantity quantity, Consumer* consumer = nullptr,
               InstrumentId instrument = 1) {
        NewOrder order;
        order.instrument = instrument;
        order.session = 1;
        order.cl_ord_id = id;
        order.side = side;
        order.price = price;
        order.qty = quantity;
        submit(Command{order}, consumer);
    }
};
struct Sink {
    std::vector<char>* bytes = nullptr;
    std::size_t* used = nullptr;
    char* prepare(std::size_t size) { return size <= bytes->size() - *used ? bytes->data() + *used : nullptr; }
    void commit(std::size_t size) { *used += size; }
};
std::string request(std::string_view type, std::string_view fields = {}, bool compact = true, bool request_id = true) {
    std::array<char, 1024> bytes{};
    protocol::fix44::Builder builder(bytes);
    builder.field(35, type);
    if (type == "V" && request_id) builder.field(262, "RECOVERY");
    std::size_t start = 0;
    while (start < fields.size()) {
        const auto end = fields.find('|', start);
        const auto item = fields.substr(start, end == std::string_view::npos ? end : end - start);
        const auto equal = item.find('=');
        std::uint64_t tag = 0;
        REQUIRE(protocol::fix44::parse_uint(item.substr(0, equal), tag));
        REQUIRE(builder.field(static_cast<unsigned>(tag), item.substr(equal + 1)));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    const auto size = builder.finish();
    REQUIRE(size);
    std::string result(bytes.data() + (compact ? 10 : 0), size - (compact ? 10 : 0));
    if (compact) {
        unsigned sum = 0;
        for (std::size_t i = 0; i < result.size() - 7; ++i) sum += static_cast<unsigned char>(result[i]);
        sum %= 256;
        result[result.size() - 4] = static_cast<char>('0' + sum / 100);
        result[result.size() - 3] = static_cast<char>('0' + sum / 10 % 10);
        result[result.size() - 2] = static_cast<char>('0' + sum % 10);
    }
    return result;
}
}  // namespace

TEST_CASE(mdp_publisher, startup_definitions_and_empty_snapshot) {
    Exchange exchange;
    REQUIRE_EQ(exchange.packets.size(), 3u);
    cme::MdpPacketView packet;
    cme::MessageView message;
    REQUIRE_EQ(cme::decode_mdp_packet(exchange.packets[1].bytes, packet), protocol::Status::ok);
    REQUIRE_EQ(cme::decode_mdp_message(packet.messages, message), protocol::Status::ok);
    REQUIRE_EQ(message.header.template_id, 54);
    CHECK_EQ(cme::load_le<std::int32_t>(message.body.data() + 55), 800001);
    CHECK_EQ(std::string_view(message.body.data() + 35, 20), "LONG-CME-SYMBOL-1234");
    Consumer consumer;
    consumer.packet(exchange.packets[2]);
    consumer.compare(exchange.engine, exchange.config.instruments);
    CHECK_EQ(consumer.books.at(800001).report, 0u);
    CHECK_EQ(exchange.publisher.ring().next_sequence(), 2u);
}

TEST_CASE(mdp_publisher, matching_cancel_replace_and_rank_changes_reconstruct) {
    Exchange exchange;
    Consumer consumer;
    exchange.order(1, Side::BUY, 10000, 5, &consumer);
    exchange.order(2, Side::BUY, 9900, 7, &consumer);
    exchange.order(3, Side::BUY, 10100, 2, &consumer);
    exchange.order(4, Side::SELL, 10100, 1, &consumer);
    CancelOrder cancel;
    cancel.session = 1;
    cancel.orig_cl_ord_id = 3;
    exchange.submit(Command{cancel}, &consumer);
    ReplaceOrder replace;
    replace.session = 1;
    replace.orig_cl_ord_id = 2;
    replace.cl_ord_id = 5;
    replace.price = 10200;
    replace.qty = 6;
    exchange.submit(Command{replace}, &consumer);
    MassCancel mass;
    mass.session = 1;
    exchange.submit(Command{mass}, &consumer);
    CHECK(consumer.books.at(800001).bids.empty());
    CHECK(consumer.books.at(800001).asks.empty());
    CHECK_EQ(consumer.end_events, 7u);
}

TEST_CASE(mdp_publisher, seeded_command_stream_matches_engine_at_every_boundary) {
    Exchange exchange;
    Consumer consumer;
    std::mt19937 random(8173);
    for (ClOrdId id = 1; id <= 500; ++id) {
        if (id % 17 == 0) {
            MassCancel cancel;
            cancel.session = 1;
            exchange.submit(Command{cancel}, &consumer);
        } else
            exchange.order(id, (random() & 1) ? Side::BUY : Side::SELL, 9000 + (random() % 21) * 100, 1 + random() % 9,
                           &consumer);
    }
}

TEST_CASE(mdp_publisher, changes_behind_the_tenth_level_publish_nothing) {
    Exchange exchange;
    Consumer consumer;
    for (ClOrdId id = 1; id <= 12; ++id)  // bids 10000 down to 8900; the tenth level is 9100
        exchange.order(id, Side::BUY, 10100 - static_cast<Price>(id) * 100, 5, &consumer);
    const auto sequence = exchange.publisher.next_sequence();
    exchange.order(13, Side::BUY, 8800, 4, &consumer);
    CHECK(exchange.packets.empty());
    CancelOrder cancel;
    cancel.session = 1;
    cancel.orig_cl_ord_id = 11;  // 9000
    exchange.submit(Command{cancel}, &consumer);
    CHECK(exchange.packets.empty());
    ReplaceOrder replace;
    replace.session = 1;
    replace.orig_cl_ord_id = 12;  // 8900 to 8700
    replace.cl_ord_id = 14;
    replace.price = 8700;
    replace.qty = 3;
    exchange.submit(Command{replace}, &consumer);
    CHECK(exchange.packets.empty());
    CHECK_EQ(exchange.publisher.next_sequence(), sequence);

    exchange.order(15, Side::BUY, 9100, 1, &consumer);  // the tenth level grows
    CHECK_EQ(exchange.packets.size(), 1u);
    cancel.orig_cl_ord_id = 1;  // the best bid goes and 8800 becomes the tenth level
    exchange.submit(Command{cancel}, &consumer);
    CHECK_EQ(exchange.packets.size(), 1u);
}

// The reference publisher also gets an AuctionSummary for every instrument in every batch, which makes it
// recompute both sides of every instrument, as the publisher did before it skipped changes behind its levels.
// A consumer checks the reference against the engine, so a rule that also kept the reference from
// recomputing still fails. Both must send the same bytes, snapshots included, while the books run deeper
// than the published levels: ten for odd seeds, three for even ones.
TEST_CASE(mdp_publisher, skipping_deep_changes_sends_the_same_bytes_as_recomputing_every_batch) {
    const server::journal::JournalSetup setup = testsupport::fuzz_setup(false, 2);
    server::ServerConfig config = mdp_config();
    config.engine.engine = setup.engine;
    config.instruments = setup.instruments;
    for (std::size_t index = 0; index < config.instruments.size(); ++index)
        config.instruments[index].cme_security_id = 800001 + static_cast<std::int32_t>(index);
    const auto instrument_count = static_cast<InstrumentId>(config.instruments.size());
    std::size_t deep_changes = 0;  // resting-book changes behind the last level of a side that shows them all
    // Few phase changes and mass cancels, so the books grow past ten levels between them.
    const testsupport::CommandWeights weights{.add = 80, .cancel = 8, .replace = 10, .mass = 1, .phase = 1};
    for (std::uint64_t seed = 1; seed <= 5; ++seed) {
        const std::uint8_t depth = seed % 2 ? 10 : 3;
        config.market_data.book_depth = depth;
        MatchingEngine engine(config.engine.engine);
        for (const auto& instrument : config.instruments) REQUIRE(engine.add_instrument(instrument) != 0);
        EventBuffer events(required_event_capacity(config.engine.engine));
        std::vector<Event> forced;
        std::vector<Packet> packets, reference_packets;
        md::MdpPublisher<Capture> publisher(config, Capture{&packets});
        md::MdpPublisher<Capture> reference(config, Capture{&reference_packets});
        Timestamp now = 1'000'000;
        REQUIRE_EQ(publisher.start(now, 1700000000000000000ull + now, engine), md::PublishStatus::Ok);
        REQUIRE_EQ(reference.start(now, 1700000000000000000ull + now, engine), md::PublishStatus::Ok);
        Consumer consumer{.depth = depth};
        for (const auto& packet : reference_packets) consumer.packet(packet);
        testsupport::CommandGenerator generator(seed, weights, 40);
        for (int step = 0; step < 4000; ++step) {
            now += 1'000'000;
            Command command = generator.next(engine);
            command.timestamp = now;
            std::array<std::array<Price, 2>, 2> last{};  // [instrument - 1][ask]; 0 when the side shows fewer
            for (InstrumentId id = 1; id <= instrument_count; ++id)
                for (const auto side : {Side::BUY, Side::SELL}) {
                    std::array<LevelInfo, 10> levels{};
                    if (engine.depth(id, side, levels.data(), depth) == depth)
                        last[id - 1][side == Side::SELL] = levels[depth - 1].price;
                }
            events.clear();
            REQUIRE_EQ(engine.handle(command, events), HandleResult::Applied);
            // A rejection naming an instrument the feed does not list would stop both feeds; it changes no book.
            const auto unlisted = [&](const Event& e) { return e.instrument > instrument_count; };
            if (std::any_of(events.begin(), events.end(), unlisted)) continue;
            forced.assign(events.begin(), events.end());
            for (InstrumentId id = 1; id <= instrument_count; ++id)
                forced.push_back(Event{EventType::AuctionSummary, id, now, {.auction_summary = {}}});
            packets.clear();
            reference_packets.clear();
            REQUIRE_EQ(publisher.on_events({events.begin(), events.size()}), md::PublishStatus::Ok);
            REQUIRE_EQ(reference.on_events(forced), md::PublishStatus::Ok);
            publisher.poll(now, UINT64_MAX);
            reference.poll(now, UINT64_MAX);
            consumer.batch_ended = false;
            for (const auto& packet : reference_packets) consumer.packet(packet);
            consumer.compare(engine, config.instruments);
            if (packets != reference_packets)
                FAIL("seed " + std::to_string(seed) + " step " + std::to_string(step) + " " +
                     testsupport::format_command(command));
            for (const auto& event : events) {
                const bool rested = event.type == EventType::Rested;
                if (!rested && !(event.type == EventType::Canceled && event.canceled.was_resting)) continue;
                const Side side = rested ? event.rested.side : event.canceled.side;
                const Price price = rested ? event.rested.price : event.canceled.price;
                const Price behind = last[event.instrument - 1][side == Side::SELL];
                deep_changes += behind != 0 && (side == Side::BUY ? price < behind : price > behind);
            }
        }
    }
    CHECK_GT(deep_changes, 1000u);
}

TEST_CASE(mdp_publisher, one_batch_changing_two_instruments_keeps_books_flags_and_report_sequences_apart) {
    constexpr std::int32_t kFirst = 800001, kSecond = 800002;
    Exchange exchange(2);
    Consumer consumer;
    exchange.order(1, Side::BUY, 10000, 5, &consumer, 1);
    exchange.order(2, Side::BUY, 9900, 3, &consumer, 1);
    exchange.order(3, Side::BUY, 9800, 2, &consumer, 1);
    exchange.order(4, Side::SELL, 10100, 4, &consumer, 2);
    exchange.order(5, Side::BUY, 10100, 1, &consumer, 2);  // trade plus a changed ask level
    CHECK_EQ(consumer.books.at(kFirst).report, 3u);
    CHECK_EQ(consumer.books.at(kSecond).report, 3u);
    CHECK_EQ(consumer.books.at(kFirst).bids.size(), 3u);
    CHECK_EQ(consumer.books.at(kSecond).asks.size(), 1u);
    const auto end_events_before = consumer.end_events;

    // One mass cancel is one batch that deletes 3 bid levels of the first instrument and the ask level of the second.
    MassCancel mass;
    mass.session = 1;
    exchange.submit(Command{mass}, &consumer);
    CHECK_EQ(exchange.packets.size(), 2u);
    CHECK_EQ(consumer.end_events, end_events_before + 1);
    CHECK(consumer.books.at(kFirst).bids.empty());
    CHECK(consumer.books.at(kSecond).asks.empty());
    CHECK_EQ(consumer.books.at(kFirst).report, 6u);
    CHECK_EQ(consumer.books.at(kSecond).report, 4u);
}

TEST_CASE(mdp_publisher, snapshot_boundary_recovers_a_missed_incremental) {
    Exchange exchange;
    Consumer consumer;
    exchange.order(1, Side::BUY, 10000, 4);  // lost by the consumer
    exchange.order(2, Side::SELL, 11000, 9);
    exchange.packets.clear();
    exchange.now += 1000000;
    exchange.publisher.poll(exchange.now, UINT64_MAX);
    bool snapshot_seen = false;
    for (const auto& packet : exchange.packets) {
        if (packet.stream != md::MdpStream::Snapshot) continue;
        consumer.packet(packet);
        snapshot_seen = true;
        cme::MdpPacketView view;
        cme::MessageView message;
        REQUIRE_EQ(cme::decode_mdp_packet(packet.bytes, view), protocol::Status::ok);
        REQUIRE_EQ(cme::decode_mdp_message(view.messages, message), protocol::Status::ok);
        CHECK_EQ(cme::load_le<std::uint32_t>(message.body.data()), exchange.publisher.next_sequence() - 1);
    }
    REQUIRE(snapshot_seen);
    consumer.compare(exchange.engine, exchange.config.instruments);
    exchange.order(3, Side::BUY, 11000, 3, &consumer);
}

TEST_CASE(mdp_publisher, unrepresentable_aggregate_stops_before_partial_event) {
    Exchange exchange;
    exchange.order(1, Side::BUY, 10000, 1000000000);
    exchange.order(2, Side::BUY, 10000, 1000000000);
    const auto sequence = exchange.publisher.next_sequence();
    exchange.packets.clear();
    exchange.events.clear();
    NewOrder order;
    order.instrument = 1;
    order.session = 1;
    order.cl_ord_id = 3;
    order.price = 10000;
    order.qty = 1000000000;
    order.ts = ++exchange.now;
    REQUIRE_EQ(exchange.engine.handle(order, exchange.events), HandleResult::Applied);
    CHECK_EQ(exchange.publisher.on_events({exchange.events.begin(), exchange.events.size()}),
             md::PublishStatus::UnrepresentableField);
    CHECK_EQ(exchange.publisher.state(), md::PublisherState::Stopped);
    CHECK_EQ(exchange.publisher.next_sequence(), sequence);
    CHECK(exchange.packets.empty());
}

TEST_CASE(mdp_replay, ring_eviction_and_exact_original_packets) {
    md::MdpReplayRing ring(2, 32);
    const std::array<char, 3> bytes{1, 2, 3};
    CHECK(!ring.append(2, bytes));
    REQUIRE(ring.append(1, bytes));
    REQUIRE(ring.append(2, bytes));
    REQUIRE(ring.append(3, bytes));
    CHECK(ring.get(1).empty());
    CHECK(ring.get(4).empty());
    CHECK_EQ(ring.first_sequence(), 2u);
    CHECK_EQ(ring.get(2).size(), 3u);
    CHECK_EQ(ring.get(3)[2], 3);
}

TEST_CASE(mdp_replay, compact_login_fragmentation_replay_and_logout) {
    Exchange exchange;
    std::vector<char> bytes(65536);
    std::size_t used = 0;
    md::MdpReplaySession<Sink> session(exchange.config.market_data, exchange.publisher.ring());
    session.open(Sink{&bytes, &used}, 0);
    const auto login = request("A", "553=CME|554=secret|");
    for (std::size_t i = 0; i < login.size(); ++i)
        CHECK_EQ(session.on_data(std::span(login).first(i), 1, 1700000000000000000ull), 0u);
    CHECK_EQ(session.on_data(login, 1, 1700000000000000000ull), login.size());
    REQUIRE(used > 2);
    CHECK_EQ(cme::load_le<std::uint16_t>(bytes.data()), used - 2);
    cme::MdpPacketView packet;
    cme::MessageView message;
    REQUIRE_EQ(cme::decode_mdp_packet(std::span(bytes).subspan(2, used - 2), packet), protocol::Status::ok);
    REQUIRE_EQ(cme::decode_mdp_message(packet.messages, message), protocol::Status::ok);
    CHECK_EQ(message.header.schema_id, 9);
    CHECK_EQ(message.header.template_id, 408);
    used = 0;
    const auto recover = request("V", "1180=1|1182=1|1183=1|");
    CHECK_EQ(session.on_data(recover, 2, 1700000000000000000ull), recover.size());
    const auto original = exchange.publisher.ring().get(1);
    REQUIRE(used > original.size() + 2);
    CHECK_EQ(std::string_view(bytes.data() + 2, original.size()), std::string_view(original.data(), original.size()));
    const auto logout = request("5");
    CHECK_EQ(session.on_data(logout, 3, 1700000000000000000ull), logout.size());
    CHECK(session.wants_close());
}

TEST_CASE(mdp_replay, invalid_credentials_ranges_checksum_and_timeout) {
    Exchange exchange;
    for (unsigned variant = 0; variant < 6; ++variant) {
        std::vector<char> bytes(65536);
        std::size_t used = 0;
        md::MdpReplaySession<Sink> session(exchange.config.market_data, exchange.publisher.ring());
        session.open(Sink{&bytes, &used}, 0);
        auto login = request("A", variant == 0 ? "553=CME|554=wrong|" : "553=CME|554=secret|", false);
        if (variant == 1) login[login.size() - 2] = 'x';
        CHECK_EQ(session.on_data(login, 1, 1700000000000000000ull), login.size());
        if (variant == 2 || variant == 3 || variant == 5) {
            used = 0;
            const auto recover =
                request("V", variant == 2 ? "1180=1|1182=1|1183=2001|" : "1180=1|1182=2|1183=2|", true, variant != 5);
            CHECK_EQ(session.on_data(recover, 2, 1700000000000000000ull), recover.size());
        } else if (variant == 4) {
            used = 0;
            session.on_timer(5000000001ull, 1700000000000000000ull);
        }
        const auto size = cme::load_le<std::uint16_t>(bytes.data());
        cme::MdpPacketView packet;
        cme::MessageView message;
        // Bad credentials/checksum reply is Logout; other variants cleared
        // their successful Logon first and now must contain only Logout.
        REQUIRE_EQ(cme::decode_mdp_packet(std::span(bytes).subspan(2, size), packet), protocol::Status::ok);
        REQUIRE_EQ(cme::decode_mdp_message(packet.messages, message), protocol::Status::ok);
        CHECK_EQ(message.header.template_id, 409);
        session.on_timer(11000000000ull, 1700000000000000000ull);
        CHECK(session.wants_close());
    }
}

TEST_CASE(mdp_publisher, publication_and_snapshot_paths_do_not_allocate) {
    if (!testfw::allocation_counting_available()) return;
    struct Count {
        std::size_t* count;
        void operator()(std::span<const char>, md::MdpStream) const noexcept { ++*count; }
    };
    auto config = mdp_config();
    MatchingEngine engine(config.engine.engine);
    REQUIRE_EQ(engine.add_instrument(config.instruments[0]), 1);
    EventBuffer events(required_event_capacity(config.engine.engine));
    std::size_t count = 0;
    md::MdpPublisher<Count> publisher(config, Count{&count});
    REQUIRE_EQ(publisher.start(1000, 1700000000000001000ull, engine), md::PublishStatus::Ok);
    NewOrder order;
    order.ts = 1001;
    order.instrument = 1;
    order.session = 1;
    order.cl_ord_id = 1;
    order.price = 10000;
    order.qty = 5;
    REQUIRE_EQ(engine.handle(order, events), HandleResult::Applied);
    std::size_t allocations = 0;
    md::PublishStatus status;
    {
        testfw::AllocationScope scope;
        status = publisher.on_events({events.begin(), events.size()});
        publisher.poll(1001001, UINT64_MAX);
        allocations = scope.count();
    }
    CHECK_EQ(status, md::PublishStatus::Ok);
    CHECK_EQ(allocations, 0u);
    CHECK(count >= 5);
}
