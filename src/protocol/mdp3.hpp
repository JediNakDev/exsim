#ifndef EXSIM_PROTOCOL_MDP3_HPP
#define EXSIM_PROTOCOL_MDP3_HPP

// CME MDP 3.0 encoders. Block lengths, offsets and null defaults come from the header
// the build generates from spec/templates_FixBinary.xml (docs/ilink-and-mdp.md).
#include "protocol/cme_sbe.hpp"
#include "protocol/mdp3_schema.hpp"

#include <algorithm>
#include <array>

namespace exsim::protocol::mdp3 {
static_assert(schema::id == 1 && schema::version == 13, "the encoders below follow schema 1 version 13");
inline constexpr std::uint8_t kEndOfEvent = 0x80;
inline constexpr std::uint8_t kLastTrade = 0x01;
inline constexpr std::int32_t kNullInt32 = INT32_MAX;

enum class UpdateAction : std::uint8_t { New = 0, Change = 1, Delete = 2, DeleteThru = 3, DeleteFrom = 4, Overlay = 5 };
enum class BookSide : char { Bid = '0', Ask = '1', Reset = 'J' };

struct BookEntry {
    std::int64_t price = cme::kNullPrice9;
    std::int32_t quantity = kNullInt32;
    std::int32_t security_id = 0;
    std::uint32_t report_sequence = 0;
    std::int32_t order_count = kNullInt32;
    std::uint8_t level = 0;
    UpdateAction action = UpdateAction::New;
    BookSide side = BookSide::Bid;
    std::int32_t tradeable_size = kNullInt32;
};

struct TradeEntry {
    std::int64_t price = 0;
    std::int32_t quantity = 0;
    std::int32_t security_id = 0;
    std::uint32_t report_sequence = 0;
    std::int32_t order_count = 0;
    std::uint8_t aggressor_side = 0;  // 0 none, 1 buy, 2 sell
    UpdateAction action = UpdateAction::New;
    std::uint32_t trade_entry_id = UINT32_MAX;
};

struct TradeOrder {
    std::uint64_t order_id = 0;
    std::int32_t quantity = 0;
};

struct SnapshotEntry {
    std::int64_t price = cme::kNullPrice9;
    std::int32_t quantity = kNullInt32;
    std::int32_t order_count = kNullInt32;
    std::int8_t level = INT8_MAX;
    std::uint16_t reference_date = UINT16_MAX;
    std::uint8_t open_close_settle = UINT8_MAX;
    std::uint8_t settlement_price_type = 0;
    char entry_type = '0';
};

struct Snapshot {
    std::uint32_t last_packet_sequence = 0;
    std::uint32_t total_reports = 0;
    std::int32_t security_id = 0;
    std::uint32_t report_sequence = 0;
    std::uint64_t transact_time = 0;
    std::uint64_t last_update_time = 0;
    std::uint16_t trade_date = UINT16_MAX;
    std::uint8_t trading_status = 17;
    std::int64_t high_limit = cme::kNullPrice9;
    std::int64_t low_limit = cme::kNullPrice9;
    std::int64_t max_variation = cme::kNullPrice9;
};

// Writes the message size, the SBE header and M's root block with its null defaults, and returns the root.
// The caller has checked that size bytes fit.
template <class M>
[[nodiscard]] char* start_message(std::span<char> output, std::size_t size) noexcept {
    cme::store_le(output.data(), static_cast<std::uint16_t>(size));
    cme::encode_header(output.data() + 2, {M::block_length, M::id, schema::id, schema::version});
    char* root = output.data() + cme::kMdpMessageHeaderSize;
    std::copy(M::defaults.begin(), M::defaults.end(), root);
    return root;
}

// Writes Group's dimension header and count entries holding the null defaults, and returns the first entry.
template <class Group>
char* start_group(char* cursor, std::uint8_t count) noexcept {
    std::copy(Group::dimension.begin(), Group::dimension.end(), cursor);
    cme::put(cursor, Group::num_in_group, count);
    cursor += Group::dimension.size();
    for (std::size_t i = 0; i < count; ++i)
        std::copy(Group::defaults.begin(), Group::defaults.end(), cursor + i * Group::block_length);
    return cursor;
}

// One message is bounded by the wire's uint8 group counts. The caller
// supplies all storage and decides packet boundaries and channel sequences.
[[nodiscard]] inline std::size_t encode_book(std::uint64_t transact_time, std::uint8_t event_indicator,
                                             std::span<const BookEntry> entries, std::span<char> output) noexcept {
    using M = schema::MDIncrementalRefreshBook46;
    using Entry = M::NoMDEntries;
    constexpr std::size_t fixed = cme::kMdpMessageHeaderSize + M::block_length + M::empty_tail.size();
    if (entries.size() > 255 || output.size() < fixed + Entry::block_length * entries.size()) return 0;
    const std::size_t size = fixed + Entry::block_length * entries.size();
    char* root = start_message<M>(output, size);
    cme::put(root, M::TransactTime, transact_time);
    cme::put(root, M::MatchEventIndicator, event_indicator);
    char* cursor = start_group<Entry>(root + M::block_length, static_cast<std::uint8_t>(entries.size()));
    for (const BookEntry& entry : entries) {
        cme::put(cursor, Entry::MDEntryPx, entry.price);
        cme::put(cursor, Entry::MDEntrySize, entry.quantity);
        cme::put(cursor, Entry::SecurityID, entry.security_id);
        cme::put(cursor, Entry::RptSeq, entry.report_sequence);
        cme::put(cursor, Entry::NumberOfOrders, entry.order_count);
        cme::put(cursor, Entry::MDPriceLevel, entry.level);
        cme::put(cursor, Entry::MDUpdateAction, static_cast<std::uint8_t>(entry.action));
        cme::put(cursor, Entry::MDEntryType, static_cast<char>(entry.side));
        cme::put(cursor, Entry::TradeableSize, entry.tradeable_size);
        cursor += Entry::block_length;
    }
    start_group<M::NoOrderIDEntries>(cursor, 0);
    return size;
}

[[nodiscard]] inline std::size_t encode_trades(std::uint64_t transact_time, std::uint8_t event_indicator,
                                               std::span<const TradeEntry> entries, std::span<const TradeOrder> orders,
                                               std::span<char> output) noexcept {
    using M = schema::MDIncrementalRefreshTradeSummary48;
    using Entry = M::NoMDEntries;
    using Order = M::NoOrderIDEntries;
    constexpr std::size_t fixed = cme::kMdpMessageHeaderSize + M::block_length + M::empty_tail.size();
    if (entries.size() > 255 || orders.size() > 255 ||
        output.size() < fixed + Entry::block_length * entries.size() + Order::block_length * orders.size())
        return 0;
    const std::size_t size = fixed + Entry::block_length * entries.size() + Order::block_length * orders.size();
    char* root = start_message<M>(output, size);
    cme::put(root, M::TransactTime, transact_time);
    cme::put(root, M::MatchEventIndicator, event_indicator);
    char* cursor = start_group<Entry>(root + M::block_length, static_cast<std::uint8_t>(entries.size()));
    for (const TradeEntry& entry : entries) {
        cme::put(cursor, Entry::MDEntryPx, entry.price);
        cme::put(cursor, Entry::MDEntrySize, entry.quantity);
        cme::put(cursor, Entry::SecurityID, entry.security_id);
        cme::put(cursor, Entry::RptSeq, entry.report_sequence);
        cme::put(cursor, Entry::NumberOfOrders, entry.order_count);
        cme::put(cursor, Entry::AggressorSide, entry.aggressor_side);
        cme::put(cursor, Entry::MDUpdateAction, static_cast<std::uint8_t>(entry.action));
        cme::put(cursor, Entry::MDTradeEntryID, entry.trade_entry_id);
        cursor += Entry::block_length;
    }
    cursor = start_group<Order>(cursor, static_cast<std::uint8_t>(orders.size()));
    for (const TradeOrder& order : orders) {
        cme::put(cursor, Order::OrderID, order.order_id);
        cme::put(cursor, Order::LastQty, order.quantity);
        cursor += Order::block_length;
    }
    return size;
}

[[nodiscard]] inline std::size_t encode_snapshot(const Snapshot& snapshot, std::span<const SnapshotEntry> entries,
                                                 std::span<char> output) noexcept {
    using M = schema::SnapshotFullRefresh52;
    using Entry = M::NoMDEntries;
    constexpr std::size_t fixed = cme::kMdpMessageHeaderSize + M::block_length + M::empty_tail.size();
    if (entries.size() > 255 || output.size() < fixed + Entry::block_length * entries.size()) return 0;
    const std::size_t size = fixed + Entry::block_length * entries.size();
    char* root = start_message<M>(output, size);
    cme::put(root, M::LastMsgSeqNumProcessed, snapshot.last_packet_sequence);
    cme::put(root, M::TotNumReports, snapshot.total_reports);
    cme::put(root, M::SecurityID, snapshot.security_id);
    cme::put(root, M::RptSeq, snapshot.report_sequence);
    cme::put(root, M::TransactTime, snapshot.transact_time);
    cme::put(root, M::LastUpdateTime, snapshot.last_update_time);
    cme::put(root, M::TradeDate, snapshot.trade_date);
    cme::put(root, M::MDSecurityTradingStatus, snapshot.trading_status);
    cme::put(root, M::HighLimitPrice, snapshot.high_limit);
    cme::put(root, M::LowLimitPrice, snapshot.low_limit);
    cme::put(root, M::MaxPriceVariation, snapshot.max_variation);
    char* cursor = start_group<Entry>(root + M::block_length, static_cast<std::uint8_t>(entries.size()));
    for (const SnapshotEntry& entry : entries) {
        cme::put(cursor, Entry::MDEntryPx, entry.price);
        cme::put(cursor, Entry::MDEntrySize, entry.quantity);
        cme::put(cursor, Entry::NumberOfOrders, entry.order_count);
        cme::put(cursor, Entry::MDPriceLevel, entry.level);
        cme::put(cursor, Entry::TradingReferenceDate, entry.reference_date);
        cme::put(cursor, Entry::OpenCloseSettlFlag, entry.open_close_settle);
        cme::put(cursor, Entry::SettlPriceType, entry.settlement_price_type);
        cme::put(cursor, Entry::MDEntryType, entry.entry_type);
        cursor += Entry::block_length;
    }
    return size;
}

[[nodiscard]] inline std::size_t encode_heartbeat(std::span<char> output) noexcept {
    using M = schema::AdminHeartbeat12;
    constexpr std::size_t size = cme::kMdpMessageHeaderSize + M::block_length;
    if (output.size() < size) return 0;
    (void)start_message<M>(output, size);
    return size;
}

[[nodiscard]] inline std::uint8_t trading_status(Phase phase) noexcept {
    switch (phase) {
        case Phase::Continuous: return 17;
        case Phase::Closed: return 4;
        case Phase::Halted: return 2;
        case Phase::PreOpen: return 21;
        case Phase::PreClose: return 24;
    }
    return 20;
}

[[nodiscard]] inline std::size_t encode_security_status(std::uint64_t timestamp, std::int32_t security_id,
                                                        std::uint16_t trade_date, Phase phase, std::uint8_t indicator,
                                                        std::span<char> output) noexcept {
    using M = schema::SecurityStatus30;
    constexpr std::size_t size = cme::kMdpMessageHeaderSize + M::block_length;
    if (output.size() < size) return 0;
    char* root = start_message<M>(output, size);
    cme::put(root, M::TransactTime, timestamp);
    cme::put(root, M::SecurityGroup, "EXSIM");
    cme::put(root, M::Asset, "EXSIM");
    cme::put(root, M::SecurityID, security_id);
    cme::put(root, M::TradeDate, trade_date);
    cme::put(root, M::MatchEventIndicator, indicator);
    cme::put(root, M::SecurityTradingStatus, trading_status(phase));
    return size;
}

[[nodiscard]] inline std::size_t encode_channel_reset(std::uint64_t timestamp, std::int16_t channel,
                                                      std::span<char> output) noexcept {
    using M = schema::ChannelReset4;
    using Entry = M::NoMDEntries;
    constexpr std::size_t size =
        cme::kMdpMessageHeaderSize + M::block_length + M::empty_tail.size() + Entry::block_length;
    if (output.size() < size) return 0;
    char* root = start_message<M>(output, size);
    cme::put(root, M::TransactTime, timestamp);
    cme::put(root, M::MatchEventIndicator, kEndOfEvent);
    cme::put(start_group<Entry>(root + M::block_length, 1), Entry::ApplID, channel);
    return size;
}

struct FutureDefinition {
    std::string_view symbol;
    std::int32_t security_id = 0;
    std::int16_t channel_id = 1;
    std::uint8_t market_segment = 1;
    std::uint8_t depth = 10;
    Phase phase = Phase::Continuous;
    std::uint32_t total_reports = 1;
    std::uint64_t timestamp = 0;
    std::uint16_t trade_date = UINT16_MAX;
    std::uint32_t lot_size = 1;
    std::uint32_t max_quantity = 99999;
    std::int64_t tick_price9 = 0;
    std::int64_t reference_price9 = 0;
    std::int64_t low_price9 = 0;
    std::int64_t high_price9 = 0;
    std::uint32_t maturity_date = 20991231;
    std::uint32_t contract_size = 1;
};

[[nodiscard]] inline std::size_t encode_future_definition(const FutureDefinition& definition,
                                                          std::span<char> output) noexcept {
    using M = schema::MDInstrumentDefinitionFuture54;
    using Feed = M::NoMDFeedTypes;
    using Attribute = M::NoInstAttrib;
    using LotRule = M::NoLotTypeRules;
    // NoEvents is empty; the other three groups hold one entry each.
    constexpr std::size_t size = cme::kMdpMessageHeaderSize + M::block_length + M::empty_tail.size() +
                                 Feed::block_length + Attribute::block_length + LotRule::block_length;
    if (definition.symbol.empty() || definition.lot_size > INT32_MAX / 10000u || output.size() < size) return 0;
    char* root = start_message<M>(output, size);
    if (!cme::put(root, M::Symbol, definition.symbol)) return 0;
    cme::put(root, M::MatchEventIndicator, kEndOfEvent);
    cme::put(root, M::TotNumReports, definition.total_reports);
    cme::put(root, M::SecurityUpdateAction, 'A');
    cme::put(root, M::LastUpdateTime, definition.timestamp);
    cme::put(root, M::MDSecurityTradingStatus, trading_status(definition.phase));
    cme::put(root, M::ApplID, definition.channel_id);
    cme::put(root, M::MarketSegmentID, definition.market_segment);
    cme::put(root, M::UnderlyingProduct, 7);  // index futures
    cme::put(root, M::SecurityExchange, "XCME");
    cme::put(root, M::SecurityGroup, "EXSIM");
    cme::put(root, M::Asset, "EXSIM");
    cme::put(root, M::SecurityID, definition.security_id);
    cme::put(root, M::SecurityType, "FUT");
    cme::put(root, M::CFICode, "FFICXX");
    // MaturityMonthYear is a uint16 year and uint8 month, day and week; the week stays null.
    char* maturity = root + M::MaturityMonthYear.offset;
    cme::store_le(maturity, static_cast<std::uint16_t>(definition.maturity_date / 10000));
    maturity[2] = static_cast<char>(definition.maturity_date / 100 % 100);
    maturity[3] = static_cast<char>(definition.maturity_date % 100);
    cme::put(root, M::Currency, "USD");
    cme::put(root, M::SettlCurrency, "USD");
    cme::put(root, M::MatchAlgorithm, 'F');  // simulator price-time matching
    cme::put(root, M::MinTradeVol, definition.lot_size);
    cme::put(root, M::MaxTradeVol, definition.max_quantity);
    cme::put(root, M::MinPriceIncrement, definition.tick_price9);
    cme::put(root, M::DisplayFactor, 1'000'000'000);
    cme::put(root, M::UnitOfMeasure, "IPNT");
    cme::put(root, M::UnitOfMeasureQty, std::int64_t{definition.contract_size} * 1'000'000'000);
    cme::put(root, M::TradingReferencePrice, definition.reference_price9);
    cme::put(root, M::HighLimitPrice, definition.high_price9);
    cme::put(root, M::LowLimitPrice, definition.low_price9);
    cme::put(root, M::UserDefinedInstrument, 'N');
    cme::put(root, M::TradingReferenceDate, definition.trade_date);
    char* feed = start_group<Feed>(start_group<M::NoEvents>(root + M::block_length, 0), 1);
    cme::put(feed, Feed::MDFeedType, "GBX");
    cme::put(feed, Feed::MarketDepth, static_cast<std::int8_t>(definition.depth));
    char* attribute = start_group<Attribute>(feed + Feed::block_length, 1);
    cme::put(attribute, Attribute::InstAttribValue, 1);
    char* lot_rule = start_group<LotRule>(attribute + Attribute::block_length, 1);
    cme::put(lot_rule, LotRule::LotType, 4);
    cme::put(lot_rule, LotRule::MinLotSize, static_cast<std::int32_t>(definition.lot_size * 10000));
    return size;
}
}  // namespace exsim::protocol::mdp3
#endif
