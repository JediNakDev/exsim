// The canonical journal written by ExchangeCore: replaying it into a fresh
// engine reproduces every dispatched event and every recorded output digest,
// damage is reported, and write failures follow `[journal] on_failure`.

#include "test/exsim/server/core/core_test_support.hpp"
#include "server/journal/journal.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace exsim;
using namespace exsim::server;
using namespace exsim::server::core_test;

namespace {

constexpr InstrumentId kAapl = 1;
constexpr InstrumentId kMsft = 2;

std::string journal_section(const std::string& path, const std::string& extra_keys = "") {
    return "\n[journal]\npath = " + path + "\n" + extra_keys;
}

// Removes the journal file when the test ends, whatever happened.
struct TempFile {
    std::string path;
    ~TempFile() { std::remove(path.c_str()); }
};

std::size_t file_size(const std::string& path) {
    struct stat info {};
    REQUIRE_EQ(::stat(path.c_str(), &info), 0);
    return static_cast<std::size_t>(info.st_size);
}

// While alive, any write that would grow a regular file past `limit_bytes`
// fails with EFBIG (SIGXFSZ is ignored). Nothing else may write to files
// meanwhile, test output included, so checks happen after it is destroyed.
class FileSizeLimit {
public:
    explicit FileSizeLimit(std::size_t limit_bytes) {
        std::cout.flush();
        std::fflush(nullptr);
        previous_handler_ = std::signal(SIGXFSZ, SIG_IGN);
        installed_ = ::getrlimit(RLIMIT_FSIZE, &previous_limit_) == 0;
        if (installed_) {
            rlimit limited = previous_limit_;
            limited.rlim_cur = static_cast<rlim_t>(limit_bytes);
            installed_ = ::setrlimit(RLIMIT_FSIZE, &limited) == 0;
        }
    }
    ~FileSizeLimit() {
        if (installed_) ::setrlimit(RLIMIT_FSIZE, &previous_limit_);
        std::signal(SIGXFSZ, previous_handler_);
    }
    FileSizeLimit(const FileSizeLimit&) = delete;
    FileSizeLimit& operator=(const FileSizeLimit&) = delete;
    [[nodiscard]] bool installed() const noexcept { return installed_; }

private:
    rlimit previous_limit_{};
    void (*previous_handler_)(int) = SIG_DFL;
    bool installed_ = false;
};

struct JournalContents {
    journal::JournalSetup setup;
    std::vector<journal::JournalRecord> records;  // records[i] has sequence number i
};

JournalContents read_journal(const std::string& path) {
    journal::JournalReader reader(path);
    JournalContents journal{reader.setup(), {}};
    while (const auto record = reader.next()) journal.records.push_back(*record);
    return journal;
}

struct Replay {
    std::vector<Command> commands;
    std::vector<Event> events;
    std::size_t output_mismatches = 0;  // records whose output digest the replay does not reproduce
};

Replay replay_journal(const std::string& path) {
    const JournalContents journal = read_journal(path);
    MatchingEngine engine(journal.setup.engine);
    for (const InstrumentConfig& instrument : journal.setup.instruments) {
        REQUIRE(engine.add_instrument(instrument) != 0);
    }
    EventBuffer output(required_event_capacity(journal.setup.engine));
    Replay replay;
    for (const journal::JournalRecord& record : journal.records) {
        replay.commands.push_back(record.command);
        output.clear();
        REQUIRE(engine.handle(record.command, output) == HandleResult::Applied);
        if (journal::output_digest({output.begin(), output.size()}) != record.output) ++replay.output_mismatches;
        replay.events.insert(replay.events.end(), output.begin(), output.end());
    }
    return replay;
}

bool same_canonical_events(const std::vector<Event>& left, const std::vector<Event>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (server::journal::encode(left[i]) != server::journal::encode(right[i])) return false;
    }
    return true;
}

bool journal_is_readable(const std::string& path) {
    try {
        (void)read_journal(path);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// The error read_journal reports for `bytes`, or "" when it reads them.
std::string read_error(const std::string& path, const std::vector<char>& bytes) {
    std::ofstream(path, std::ios::binary | std::ios::trunc)
        .write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    try {
        (void)read_journal(path);
        return "";
    } catch (const std::exception& error) {
        return error.what();
    }
}

}  // namespace

TEST_CASE(server_core_journal, replay_reproduces_every_dispatched_event) {
    const TempFile journal{temp_journal_path("replay")};
    RecordingHarness h(parse_test_config(std::string{} + journal_section(journal.path)));
    REQUIRE(h.core.journal_state() == JournalState::Healthy);
    REQUIRE(h.core.bind(1, 11));
    REQUIRE(h.core.bind(2, 12));

    REQUIRE_EQ(h.core.submit(limit_order(2, 9, kAapl, Side::SELL, 1'500'000, 100)), SubmitResult::Applied);
    // Clients, admin and a disconnect, interleaved on one clock.
    for (Timestamp now = kSecond; now < 2 * kSecond; now += kMillisecond) {
        h.clock.day = now;
        h.core.timer(now);
        if (now == kSecond + 300 * kMillisecond) {
            REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'400'000, 100)), SubmitResult::Applied);
            REQUIRE_EQ(h.core.submit(limit_order(1, 2, kMsft, Side::BUY, 4'000'000, 30)), SubmitResult::Applied);
            REQUIRE_EQ(h.core.submit(limit_order(2, 1, kMsft, Side::SELL, 3'990'000, 20)), SubmitResult::Applied);
        }
        if (now == kSecond + 500 * kMillisecond) {
            std::string reply;
            CoreAdmin(h.core).execute_line("phase MSFT continuous", reply);
            REQUIRE(ends_with_ok(reply));
        }
        if (now == kSecond + 700 * kMillisecond) {
            const auto best_ask = h.core.engine().best_ask(kAapl);
            REQUIRE(best_ask.has_value());
            REQUIRE_EQ(h.core.submit(limit_order(2, 2, kAapl, Side::BUY, *best_ask, 10)), SubmitResult::Applied);
            h.core.unbind(1, 11);  // cancel-on-disconnect
        }
    }
    h.core.shutdown();  // SetPhase Closed, then a journal flush
    REQUIRE(h.core.journal_state() == JournalState::Healthy);

    const Replay replay = replay_journal(journal.path);
    CHECK_EQ(replay.commands.size(), h.core.counters().journal_records);
    CHECK_EQ(replay.commands.size(), h.core.counters().commands_applied);
    CHECK_EQ(replay.events.size(), h.recorder.events.size());
    CHECK(same_canonical_events(replay.events, h.recorder.events));
    CHECK_EQ(replay.output_mismatches, 0u);

    bool saw_cancel_on_disconnect = false;
    for (const Command& command : replay.commands) {
        saw_cancel_on_disconnect = saw_cancel_on_disconnect || (command.type == CommandType::MassCancel &&
                                                                command.mass_cancel.cancel_on_disconnect);
    }
    CHECK(saw_cancel_on_disconnect);
    CHECK(replay.commands.back().type == CommandType::SetPhase);
}

TEST_CASE(server_core_journal, refused_commands_are_not_journaled) {
    const TempFile journal{temp_journal_path("refused")};
    RecordingHarness h(parse_test_config("[exchange]\ntrading_day_start = 00:00:02\n" + journal_section(journal.path)));
    h.clock.day = kSecond;
    CHECK_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::OutsideTradingDay);
    h.clock.day = 3 * kSecond;
    CHECK_EQ(h.core.submit(limit_order(1, 2, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    REQUIRE(h.core.flush_journal());
    const Replay replay = replay_journal(journal.path);
    REQUIRE_EQ(replay.commands.size(), 1u);
    CHECK_EQ(replay.commands[0].new_order.cl_ord_id, 2u);
    CHECK_EQ(replay.commands[0].timestamp, 3 * kSecond);  // the stamp is journaled
}

TEST_CASE(server_core_journal, timer_flushes_periodically_so_the_file_is_readable_while_running) {
    const TempFile journal{temp_journal_path("periodic")};
    RecordingHarness h(parse_test_config(journal_section(journal.path, "flush_interval_ms = 500\n")));
    h.clock.day = 100 * kMillisecond;
    h.core.timer(h.clock.day);  // less than one interval since midnight: no flush yet
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    CHECK(!journal_is_readable(journal.path));  // records still buffered, no footer

    h.clock.day = 600 * kMillisecond;
    h.core.timer(h.clock.day);
    REQUIRE(journal_is_readable(journal.path));
    CHECK_EQ(replay_journal(journal.path).commands.size(), 1u);

    REQUIRE_EQ(h.core.submit(limit_order(1, 2, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    h.clock.day = 900 * kMillisecond;
    h.core.timer(h.clock.day);  // within the interval: the file still ends at the last flush
    CHECK_EQ(replay_journal(journal.path).commands.size(), 1u);
    h.clock.day = 1100 * kMillisecond;
    h.core.timer(h.clock.day);
    CHECK_EQ(replay_journal(journal.path).commands.size(), 2u);
}

TEST_CASE(server_core_journal, zero_flush_interval_never_flushes_from_the_timer) {
    const TempFile journal{temp_journal_path("no-periodic")};
    RecordingHarness h(parse_test_config(journal_section(journal.path, "flush_interval_ms = 0\n")));
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    h.core.timer(10 * kSecond);
    CHECK(!journal_is_readable(journal.path));
    REQUIRE(h.core.flush_journal());
    CHECK(journal_is_readable(journal.path));
}

TEST_CASE(server_core_journal, failed_flush_with_shutdown_policy_closes_the_exchange) {
    const TempFile journal{temp_journal_path("fail-shutdown")};
    RecordingHarness h(parse_test_config(journal_section(journal.path)));
    for (ClOrdId token = 1; token <= 3; ++token) {
        REQUIRE_EQ(h.core.submit(limit_order(1, token, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    }
    bool flushed = true;
    bool limit_installed = false;
    {
        const FileSizeLimit limit(file_size(journal.path));
        limit_installed = limit.installed();
        flushed = h.core.flush_journal();
    }
    REQUIRE(limit_installed);
    CHECK(!flushed);
    CHECK(h.core.journal_state() == JournalState::Failed);
    CHECK_EQ(h.core.counters().journal_failures, 1u);
    CHECK_EQ(h.core.counters().journal_records, 3u);
    // Shutdown ran: every instrument closed through submit (not journaled).
    CHECK(h.core.shutdown_requested());
    CHECK(h.core.engine().phase(kAapl) == Phase::Closed);
    CHECK_EQ(h.core.counters().commands_applied, 4u);
    CHECK_EQ(h.recorder.sessions_shutdown_calls, 1u);
    CHECK(!h.core.flush_journal());  // failure is latched
    std::string reply;
    CoreAdmin(h.core).execute_line("status", reply);
    CHECK(contains(reply, "journal state=failed path=" + journal.path + " records=3 failures=1 on_failure=shutdown\n"));
}

TEST_CASE(server_core_journal, failed_append_with_shutdown_policy_finishes_the_batch_first) {
    const TempFile journal{temp_journal_path("fail-append")};
    RecordingHarness h(parse_test_config(journal_section(journal.path, "flush_interval_ms = 0\n")));
    std::vector<SubmitResult> results;
    std::vector<bool> shut_down_after;
    std::size_t batches_before_failure = 0;
    {
        const FileSizeLimit limit(file_size(journal.path));
        // 128 records fit the writer's buffer; the 129th append writes it out.
        for (ClOrdId token = 1; token <= 130; ++token) {
            if (token == 129) batches_before_failure = h.recorder.batch_count();
            results.push_back(h.core.submit(limit_order(1, token, kAapl, Side::BUY, 1'000'000, 10)));
            shut_down_after.push_back(h.core.shutdown_requested());
        }
    }
    REQUIRE_EQ(results.size(), 130u);
    CHECK(!shut_down_after[127]);
    CHECK(shut_down_after[128]);
    CHECK_EQ(results[128], SubmitResult::Applied);  // applied and dispatched before the shutdown
    CHECK(h.recorder.batch(batches_before_failure)[0].type == EventType::Accepted);
    CHECK(h.recorder.batch(batches_before_failure + 1)[0].type == EventType::Canceled);  // EndOfDay expiry
    CHECK_EQ(h.core.counters().journal_records, 128u);
    CHECK_EQ(h.core.counters().journal_failures, 1u);
    CHECK(h.core.engine().phase(kAapl) == Phase::Closed);
}

TEST_CASE(server_core_journal, failed_append_with_continue_policy_keeps_trading) {
    const TempFile journal{temp_journal_path("fail-continue")};
    RecordingHarness h(parse_test_config(journal_section(journal.path, "on_failure = continue\n")));
    std::vector<SubmitResult> results;
    {
        const FileSizeLimit limit(file_size(journal.path));
        for (ClOrdId token = 1; token <= 140; ++token) {
            results.push_back(h.core.submit(limit_order(1, token, kAapl, Side::BUY, 1'000'000, 10)));
        }
        h.core.timer(5 * kSecond);  // periodic flush is skipped once failed
    }
    for (const SubmitResult result : results) CHECK_EQ(result, SubmitResult::Applied);
    CHECK(!h.core.shutdown_requested());
    CHECK(h.core.journal_state() == JournalState::Failed);
    CHECK_EQ(h.core.counters().journal_records, 128u);
    CHECK_EQ(h.core.counters().journal_failures, 1u);
    CHECK_EQ(h.core.engine().live_order_count(), 140u);
    CHECK(!h.core.flush_journal());
    CHECK(!journal_is_readable(journal.path));
}

TEST_CASE(server_core_journal, disabled_journal_reports_disabled) {
    RecordingHarness h(parse_test_config());
    CHECK(h.core.journal_state() == JournalState::Disabled);
    CHECK(!h.core.flush_journal());
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    CHECK_EQ(h.core.counters().journal_records, 0u);
    CHECK(!h.core.shutdown_requested());
}

TEST_CASE(server_core_journal, header_preserves_full_instrument_identity_and_records_are_compact) {
    const TempFile file{temp_journal_path("identity")};
    auto config = parse_test_config();
    auto& instrument = config.instruments.front();
    instrument.cme_security_id = -123;
    instrument.cme_symbol.fill('X');
    instrument.cme_maturity_date = 20301220;
    instrument.cme_contract_size = 50;
    config.engine.engine.max_level_qty = 12345;  // replay must refuse the same orders
    journal::JournalWriter writer(file.path, {config.engine.engine, config.instruments});
    const auto header_size = file_size(file.path);
    REQUIRE(writer.flush());
    CHECK_EQ(file_size(file.path), header_size + 24);
    const auto before = file_size(file.path);
    REQUIRE(writer.append(Command{SetPhase{123, 1, Phase::Halted}}, 0x0123456789abcdefull));
    // Append remains buffered; a successful flush makes it readable by another fd.
    CHECK_EQ(file_size(file.path), before);
    REQUIRE(writer.flush());
    CHECK_EQ(file_size(file.path), before + 64);
    const JournalContents read = read_journal(file.path);
    CHECK_EQ(read.setup.engine.max_level_qty, std::uint64_t{12345});
    const auto& actual = read.setup.instruments.front();
    CHECK(actual.symbol == instrument.symbol);
    CHECK_EQ(actual.tick_size, instrument.tick_size);
    CHECK_EQ(actual.lot_size, instrument.lot_size);
    CHECK_EQ(actual.max_order_qty, instrument.max_order_qty);
    CHECK_EQ(actual.min_price, instrument.min_price);
    CHECK_EQ(actual.max_price, instrument.max_price);
    CHECK_EQ(actual.reference_price, instrument.reference_price);
    CHECK(actual.initial_phase == instrument.initial_phase);
    CHECK_EQ(actual.cme_security_id, instrument.cme_security_id);
    CHECK(actual.cme_symbol == instrument.cme_symbol);
    CHECK_EQ(actual.cme_maturity_date, instrument.cme_maturity_date);
    CHECK_EQ(actual.cme_contract_size, instrument.cme_contract_size);
    REQUIRE_EQ(read.records.size(), 1u);
    CHECK(read.records[0].command.type == CommandType::SetPhase);
    CHECK_EQ(read.records[0].command.ts(), 123u);
    CHECK_EQ(read.records[0].output, 0x0123456789abcdefull);
    REQUIRE(writer.append(limit_order(1, 9, 1, Side::BUY, 1'000'000, 10), 7));
    REQUIRE(writer.flush());
    CHECK_EQ(file_size(file.path), before + 64 + journal::kMaxRecordBytes);
    const JournalContents updated = read_journal(file.path);
    REQUIRE_EQ(updated.records.size(), 2u);
    CHECK(updated.records[1].command.type == CommandType::NewOrder);
    CHECK_EQ(updated.records[1].output, 7u);
}

TEST_CASE(server_core_journal, variable_records_roundtrip_all_command_types_and_detect_corruption) {
    const TempFile file{temp_journal_path("records")};
    const auto config = parse_test_config();
    journal::JournalWriter writer(file.path, {config.engine.engine, config.instruments});
    const auto header = file_size(file.path);
    // No field at its default, so a field the reader skips or decodes as a constant fails.
    const Command commands[] = {Command{NewOrder{1, 2, 3, 4, 5, Side::SELL, OrdType::Market, TimeInForce::IOC,
                                                 StpMode::CancelBoth, true, -6, 7}},
                                Command{CancelOrder{8, 9, 10, 11, 12}},
                                Command{ReplaceOrder{13, 14, 15, 16, -17, 18, true}},
                                Command{MassCancel{19, 20, 21, 22, true}}, Command{SetPhase{23, 24, Phase::PreClose}}};
    const std::size_t sizes[] = {136, 80, 96, 80, 64};
    auto expected = header + 24;
    for (std::size_t index = 0; index < std::size(commands); ++index) {
        REQUIRE(writer.append(commands[index], index));
        REQUIRE(writer.flush());
        expected += sizes[index];
        CHECK_EQ(file_size(file.path), expected);
    }
    const JournalContents read = read_journal(file.path);
    REQUIRE_EQ(read.records.size(), std::size(commands));
    for (std::size_t index = 0; index < std::size(commands); ++index) {
        CHECK(journal::encode(read.records[index].command) == journal::encode(commands[index]));
        CHECK_EQ(read.records[index].output, index);
    }
    // A length beyond the remaining file must fail before any command escapes.
    {
        std::fstream corrupt(file.path, std::ios::in | std::ios::out | std::ios::binary);
        corrupt.seekp(static_cast<std::streamoff>(header + 8));
        const char invalid_length[8] = {127, 0, 0, 0, 0, 0, 0, 0};
        corrupt.write(invalid_length, 8);
    }
    CHECK(!journal_is_readable(file.path));
}

TEST_CASE(server_core_journal, short_record_read_names_the_record_and_offset) {
    // The file shrinks after the reader checked its footer, as a failing disk could leave a
    // read. The cut is far past the start so the stdio buffer cannot hold the lost record.
    // 8 bytes in stops the 16-byte prefix read; 24 bytes in stops the rest of the record.
    const TempFile file{temp_journal_path("shrunk")};
    const auto config = parse_test_config();
    for (const std::size_t into : {std::size_t{8}, std::size_t{24}}) {
        std::size_t header = 0;
        {
            journal::JournalWriter writer(file.path, {config.engine.engine, config.instruments});
            header = file_size(file.path);
            for (std::uint64_t index = 0; index < 1000; ++index) {
                REQUIRE(writer.append(Command{NewOrder{}}, index));
                if (index % 100 == 99) REQUIRE(writer.flush());
            }
        }
        const std::size_t cut = header + 900 * 136;
        journal::JournalReader reader(file.path);
        REQUIRE(::truncate(file.path.c_str(), static_cast<off_t>(cut + into)) == 0);
        std::string error;
        try {
            while (reader.next()) {}
        } catch (const std::runtime_error& e) {
            error = e.what();
        }
        CHECK_EQ(error, "cannot read in record 900 at byte " + std::to_string(cut));
    }
}

TEST_CASE(server_core_journal, symlinked_destination_is_refused_and_its_target_is_unchanged) {
    const TempFile target{temp_journal_path("symlink-target")};
    const TempFile link{temp_journal_path("symlink")};
    std::ofstream(target.path) << "sentinel";
    REQUIRE_EQ(::symlink(target.path.c_str(), link.path.c_str()), 0);
    const auto config = parse_test_config();
    bool refused = false;
    try {
        journal::JournalWriter writer(link.path, {config.engine.engine, config.instruments});
    } catch (const std::exception&) {
        refused = true;
    }
    CHECK(refused);
    std::ifstream contents(target.path);
    CHECK(std::string(std::istreambuf_iterator<char>(contents), {}) == "sentinel");
}

TEST_CASE(server_core_journal, second_writer_is_refused_without_truncating_and_a_restart_works) {
    const TempFile file{temp_journal_path("shared")};
    const auto config = parse_test_config();
    {
        journal::JournalWriter first(file.path, {config.engine.engine, config.instruments});
        REQUIRE(first.append(Command{SetPhase{123, 1, Phase::Halted}}, 0));
        REQUIRE(first.flush());
        const auto written = file_size(file.path);
        bool refused = false;
        try {
            journal::JournalWriter second(file.path, {config.engine.engine, config.instruments});
        } catch (const std::exception&) {
            refused = true;
        }
        CHECK(refused);
        CHECK_EQ(file_size(file.path), written);
    }
    // Once the first writer is gone, the same file can be truncated and reused.
    journal::JournalWriter restarted(file.path, {config.engine.engine, config.instruments});
    REQUIRE(restarted.flush());
    CHECK(read_journal(file.path).records.empty());
}

TEST_CASE(server_core_journal, every_truncation_and_byte_flip_is_reported) {
    const TempFile file{temp_journal_path("damage")};
    const auto config = parse_test_config();
    {
        journal::JournalWriter writer(file.path, {config.engine.engine, config.instruments});
        REQUIRE(writer.append(limit_order(1, 1, kAapl, Side::BUY, 1'000'000, 10), 11));
        REQUIRE(writer.append(Command{CancelOrder{}}, 12));
        REQUIRE(writer.append(Command{SetPhase{5, 0, Phase::Closed}}, 13));
        REQUIRE(writer.flush());
    }
    std::ifstream in(file.path, std::ios::binary);
    const std::vector<char> good{std::istreambuf_iterator<char>(in), {}};
    REQUIRE_EQ(read_error(file.path, good), "");
    std::size_t unreported_cuts = 0;
    for (std::size_t size = 0; size < good.size(); ++size) {
        const std::vector<char> cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(size));
        if (read_error(file.path, cut).find("truncated") == std::string::npos) ++unreported_cuts;
    }
    CHECK_EQ(unreported_cuts, 0u);
    std::size_t unreported_flips = 0;
    for (std::size_t index = 0; index < good.size(); ++index) {
        std::vector<char> damaged = good;
        damaged[index] = static_cast<char>(damaged[index] ^ 0x40);
        if (read_error(file.path, damaged).empty()) ++unreported_flips;
    }
    CHECK_EQ(unreported_flips, 0u);
    // A damaged record is named by its sequence number.
    std::vector<char> damaged = good;
    damaged[good.size() - 24 - 8] ^= 0x01;  // checksum of the last record
    CHECK_EQ(read_error(file.path, damaged).find("bad checksum in record 2 at byte"), 0u);
}

TEST_CASE(server_core_journal, output_digest_sees_the_same_top_bit_change_in_two_events) {
    // A resting order produces Accepted and Rested with one timestamp. With word-wise FNV-1a,
    // flipping bit 63 in both cancelled out and replay matched two wrong timestamps.
    std::array<Event, 2> events{};
    events[0].type = EventType::Accepted;
    events[0].accepted = AcceptedEvent{};
    events[1].type = EventType::Rested;
    events[1].rested = RestedEvent{};
    for (Event& event : events) event.ts = 5;
    const std::uint64_t digest = journal::output_digest(events);
    for (Event& event : events) event.ts ^= Timestamp{1} << 63;
    CHECK(journal::output_digest(events) != digest);
}

TEST_CASE(server_core_journal, output_digest_sees_an_extra_event_whose_words_are_all_zero) {
    // Accepted is tag 0, so an Accepted event with every field zero folded only zero words
    // into a digest that started at zero, and an extra one at the start of a batch went unseen.
    std::array<Event, 2> events{};
    events[0].type = EventType::Accepted;
    events[0].accepted = AcceptedEvent{};
    events[1].type = EventType::Rested;
    events[1].rested = RestedEvent{};
    events[1].ts = 5;
    const std::span<const Event> all(events);
    CHECK(journal::output_digest(all) != journal::output_digest(all.subspan(1)));
    CHECK(journal::output_digest(all.first(1)) != journal::output_digest({}));
}

TEST_CASE(server_core_journal, output_digest_of_a_resting_order_is_pinned) {
    // Journals outlive builds: a digest change makes every existing schema 3 journal diverge
    // at sequence 0, while tests that write and replay with one build still pass.
    std::array<Event, 2> events{};
    events[0].type = EventType::Accepted;
    events[0].accepted =
        AcceptedEvent{7, 3, 2, 5, Side::SELL, OrdType::Limit, TimeInForce::Day, StpMode::None, false, 1'502'500, 60};
    events[1].type = EventType::Rested;
    events[1].rested = RestedEvent{7, 3, 2, Side::SELL, 1'502'500, 60};
    for (Event& event : events) {
        event.instrument = 1;
        event.ts = 57'488'377'589'000;
    }
    CHECK_EQ(journal::output_digest(events), 0xce55d6711f256508ull);
}

TEST_CASE(server_core_journal, header_word_wider_than_its_field_is_reported) {
    const TempFile file{temp_journal_path("width")};
    const auto config = parse_test_config();
    {
        journal::JournalWriter writer(file.path, {config.engine.engine, config.instruments});
        REQUIRE(writer.flush());
    }
    std::ifstream in(file.path, std::ios::binary);
    std::vector<char> bytes{std::istreambuf_iterator<char>(in), {}};
    // The first instrument (at byte 48, 108 bytes each) has its CME contract size at offset 100.
    // Give that word a nonzero upper half and reseal the header checksum.
    bytes[48 + 100 + 3] = 1;
    const std::size_t header = 48 + config.instruments.size() * 108;
    auto* raw = reinterpret_cast<unsigned char*>(bytes.data());
    journal::store64(raw + header, journal::hash_bytes({raw, header}));
    CHECK_EQ(read_error(file.path, bytes), "instrument field width");
}

TEST_CASE(server_core_journal, only_regular_files_are_read) {
    // Opened without blocking and checked on the descriptor, so a FIFO cannot block the reader
    // and a device cannot feed it forever.
    const TempFile fifo{temp_journal_path("fifo")};
    REQUIRE_EQ(::mkfifo(fifo.path.c_str(), 0600), 0);
    for (const std::string& path : {std::string("/tmp"), std::string("/dev/null"), fifo.path}) {
        std::string error;
        try {
            (void)read_journal(path);
        } catch (const std::exception& thrown) {
            error = thrown.what();
        }
        CHECK_EQ(error.find("cannot open"), 0u);
    }
}

TEST_CASE(server_core_journal, huge_file_that_is_not_a_journal_is_reported_from_its_first_bytes) {
    // Holes up to a petabyte, as large as the filesystem allows (ext4 stops below 16 TiB): the
    // reader must check what it reads before sizing memory by the file.
    const TempFile file{temp_journal_path("sparse")};
    std::ofstream(file.path) << "not a journal";
    int shift = 50;
    while (::truncate(file.path.c_str(), off_t{1} << shift) != 0) {
        REQUIRE(errno == EFBIG && shift > 40);
        --shift;
    }
    std::string error;
    try {
        (void)read_journal(file.path);
    } catch (const std::exception& thrown) {
        error = thrown.what();
    }
    CHECK_EQ(error, "not a journal");
}
