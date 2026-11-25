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
    bool append(const Command&) noexcept {
        probe->appended_commands.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    bool flush() noexcept {
        probe->writer_inside_flush = true;
        probe->writer_inside_flush.notify_one();
        probe->stall_flush.wait(true);  // sleeps until release_flush
        probe->flush_requests.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    std::shared_ptr<Probe> probe;
};

void release_flush(Probe& probe) {
    probe.stall_flush = false;
    probe.stall_flush.notify_one();
}

using TestJournalWorker = server::runtime::JournalWorker<CountingJournalWriter, 1024>;

}  // namespace

TEST_CASE(journal_worker, destructor_drains_accepted_entries_and_joins) {
    const auto probe = new_probe();
    probe->stall_flush = true;
    // A loaded machine can keep the writer off the CPU for longer than the default 100 ms.
    constexpr std::chrono::seconds shutdown_wait{10};
    std::optional<TestJournalWorker> worker;
    worker.emplace("unused", server::journal::JournalSetup{}, shutdown_wait);
    // Hold the writer inside a flush while the entries queue, so shutdown has them all to drain.
    REQUIRE(worker->flush());
    probe->writer_inside_flush.wait(false);
    for (int index = 0; index < 1000; ++index) REQUIRE(worker->append(Command{}));
    REQUIRE(worker->flush());
    release_flush(*probe);
    const auto started = std::chrono::steady_clock::now();
    worker.reset();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK_EQ(probe->appended_commands.load(), 1000u);
    CHECK_EQ(probe->flush_requests.load(), 3u);  // the held flush, the requested one and the final one
    CHECK(elapsed < shutdown_wait / 2);          // joined on the writer's signal, well before the wait runs out
}

TEST_CASE(journal_worker, ring_holds_capacity_minus_one_entries_then_fails) {
    const auto probe = new_probe();
    probe->stall_flush = true;
    {
        TestJournalWorker worker("unused", server::journal::JournalSetup{});
        REQUIRE(worker.flush());
        probe->writer_inside_flush.wait(false);
        std::size_t accepted = 0;
        while (worker.append(Command{})) ++accepted;
        CHECK_EQ(accepted, 1023u);
        CHECK(!worker.healthy());
        release_flush(*probe);
    }
}

TEST_CASE(journal_worker, stalled_writer_is_abandoned_after_the_shutdown_wait) {
    const auto probe = new_probe();
    probe->stall_flush = true;
    std::optional<TestJournalWorker> worker;
    worker.emplace("unused", server::journal::JournalSetup{});
    REQUIRE(worker->flush());
    probe->writer_inside_flush.wait(false);
    const auto started = std::chrono::steady_clock::now();
    worker.reset();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed >= std::chrono::milliseconds(100));
    CHECK(elapsed < std::chrono::milliseconds(1000));
    release_flush(*probe);  // the detached writer finishes on the state it owns
    while (probe->flush_requests.load() < 2) std::this_thread::yield();
}

TEST_CASE(journal_worker, wakes_for_each_request_after_becoming_idle) {
    const auto probe = new_probe();
    TestJournalWorker worker("unused", server::journal::JournalSetup{});
    for (int index = 0; index < 20; ++index) {
        REQUIRE(worker.append(Command{}));
        REQUIRE(worker.flush());
        // Only a hang guard: a lost wakeup never drains, however long the wait.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!worker.drained() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        REQUIRE(worker.drained());
        CHECK_EQ(probe->appended_commands.load(), static_cast<std::uint64_t>(index + 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
