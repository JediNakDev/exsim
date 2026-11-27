// runtime::JournalWorker: shutdown drains every accepted entry and waits for
// the writer thread without polling; a stalled writer is abandoned.

#include "server/runtime/journal_worker.hpp"
#include "test/framework/test.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

using namespace exsim;

namespace {

// What one test's writer did. The writer keeps its Probe alive, so a writer
// abandoned by one test's shutdown wait cannot change what a later test reads.
struct Probe {
    std::atomic<std::uint64_t> appended_commands{0};
    std::atomic<std::uint64_t> last_output{0};
    std::atomic<std::uint64_t> flush_requests{0};
    std::atomic<bool> stall_flush{false};
    std::atomic<bool> writer_inside_flush{false};
};

// The worker constructs its writer on the test thread; the writer takes this.
std::shared_ptr<Probe> next_probe;

std::shared_ptr<Probe> new_probe() {
    next_probe = std::make_shared<Probe>();
    return next_probe;
}

struct CountingJournalWriter {
    CountingJournalWriter(const std::string&, const server::journal::JournalSetup&) : probe(next_probe) {}
    bool append(const Command&, std::uint64_t output) noexcept {
        probe->appended_commands.fetch_add(1, std::memory_order_relaxed);
        probe->last_output.store(output, std::memory_order_relaxed);
        return true;
    }
    bool flush() noexcept {
        probe->writer_inside_flush = true;
        probe->stall_flush.wait(true);  // sleeps while a FlushGate holds it
        probe->flush_requests.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    std::shared_ptr<Probe> probe;
};

// Holds the writer inside flush() until open() or the end of the test, so a
// failed REQUIRE never leaves it blocked. Declare it after the worker.
struct FlushGate {
    explicit FlushGate(Probe& held) : probe(held) { probe.stall_flush = true; }
    ~FlushGate() { open(); }
    void open() {
        probe.stall_flush = false;
        probe.stall_flush.notify_one();
    }
    Probe& probe;
};

// Long enough for a writer starved of CPU on a loaded machine; only a regression waits it out.
constexpr std::chrono::seconds patience{10};

// A lost wakeup never finishes, so this bound fails the test instead of hanging the suite.
template <class Done>
bool eventually(Done done) {
    const auto deadline = std::chrono::steady_clock::now() + patience;
    while (!done() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    return done();
}

using TestJournalWorker = server::runtime::JournalWorker<CountingJournalWriter, 1024>;

}  // namespace

TEST_CASE(journal_worker, destructor_drains_accepted_entries_and_joins) {
    const auto probe = new_probe();
    std::optional<TestJournalWorker> worker;
    worker.emplace("unused", server::journal::JournalSetup{}, patience);
    for (std::uint64_t index = 0; index < 1000; ++index) REQUIRE(worker->append(Command{}, index));
    REQUIRE(worker->flush());
    const auto started = std::chrono::steady_clock::now();
    worker.reset();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK_EQ(probe->appended_commands.load(), 1000u);
    CHECK_EQ(probe->last_output.load(), 999u);   // each entry carries its command's output digest
    CHECK_EQ(probe->flush_requests.load(), 2u);  // the requested flush and the final one
    CHECK(elapsed < patience / 2);               // joined on the writer's signal, well before the wait runs out
}

TEST_CASE(journal_worker, ring_holds_capacity_minus_one_entries_then_fails) {
    const auto probe = new_probe();
    {
        TestJournalWorker worker("unused", server::journal::JournalSetup{}, patience);
        FlushGate gate(*probe);
        REQUIRE(worker.flush());
        REQUIRE(eventually([&] { return probe->writer_inside_flush.load(); }));
        std::size_t accepted = 0;
        while (worker.append(Command{}, 0)) ++accepted;
        CHECK_EQ(accepted, 1023u);
        CHECK(!worker.healthy());
    }
    // The overflow stopped the worker while its writer was held, so the writer
    // drained every accepted entry after the stop.
    CHECK_EQ(probe->appended_commands.load(), 1023u);
    CHECK_EQ(probe->flush_requests.load(), 2u);  // the held flush and the final one
}

TEST_CASE(journal_worker, stalled_writer_is_abandoned_after_the_shutdown_wait) {
    const auto probe = new_probe();
    std::optional<TestJournalWorker> worker;
    worker.emplace("unused", server::journal::JournalSetup{});
    FlushGate gate(*probe);
    REQUIRE(worker->flush());
    REQUIRE(eventually([&] { return probe->writer_inside_flush.load(); }));
    const auto started = std::chrono::steady_clock::now();
    worker.reset();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed >= std::chrono::milliseconds(100));
    CHECK(elapsed < std::chrono::milliseconds(1000));
    gate.open();  // the detached writer finishes on the state it owns
    CHECK(eventually([&] { return probe->flush_requests.load() == 2; }));
}

TEST_CASE(journal_worker, wakes_for_each_request_after_becoming_idle) {
    const auto probe = new_probe();
    TestJournalWorker worker("unused", server::journal::JournalSetup{});
    for (int index = 0; index < 20; ++index) {
        REQUIRE(worker.append(Command{}, 0));
        REQUIRE(worker.flush());
        REQUIRE(eventually([&] { return worker.drained(); }));
        CHECK_EQ(probe->appended_commands.load(), static_cast<std::uint64_t>(index + 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
