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
        probe->writer_inside_flush.store(true, std::memory_order_release);
        while (probe->stall_flush.load(std::memory_order_acquire)) std::this_thread::yield();
        probe->flush_requests.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    std::shared_ptr<Probe> probe;
};

using TestJournalWorker = server::runtime::JournalWorker<CountingJournalWriter, 1024>;

}  // namespace

TEST_CASE(journal_worker, destructor_drains_accepted_entries_and_joins) {
    const auto probe = new_probe();
    std::optional<TestJournalWorker> worker;
    worker.emplace("unused", server::journal::JournalSetup{});
    // A new thread can wait long on a loaded machine; the shutdown wait is
    // for a running writer, so start the test once the writer has run.
    REQUIRE(worker->append(Command{}));
    while (probe->appended_commands.load() == 0) std::this_thread::yield();
    for (int index = 1; index < 1000; ++index) REQUIRE(worker->append(Command{}));
    REQUIRE(worker->flush());
    const auto started = std::chrono::steady_clock::now();
    worker.reset();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK_EQ(probe->appended_commands.load(), 1000u);
    CHECK_EQ(probe->flush_requests.load(), 2u);  // the requested flush and the final one
    CHECK(elapsed < std::chrono::milliseconds(100));  // joined on the writer's signal, not the timeout
}

TEST_CASE(journal_worker, ring_holds_capacity_minus_one_entries_then_fails) {
    const auto probe = new_probe();
    probe->stall_flush = true;
    {
        TestJournalWorker worker("unused", server::journal::JournalSetup{});
        REQUIRE(worker.flush());
        while (!probe->writer_inside_flush.load(std::memory_order_acquire)) std::this_thread::yield();
        std::size_t accepted = 0;
        while (worker.append(Command{})) ++accepted;
        CHECK_EQ(accepted, 1023u);
        CHECK(!worker.healthy());
        probe->stall_flush = false;
    }
}

TEST_CASE(journal_worker, stalled_writer_is_abandoned_after_the_shutdown_wait) {
    const auto probe = new_probe();
    probe->stall_flush = true;
    std::optional<TestJournalWorker> worker;
    worker.emplace("unused", server::journal::JournalSetup{});
    REQUIRE(worker->flush());
    while (!probe->writer_inside_flush.load(std::memory_order_acquire)) std::this_thread::yield();
    const auto started = std::chrono::steady_clock::now();
    worker.reset();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed >= std::chrono::milliseconds(100));
    CHECK(elapsed < std::chrono::milliseconds(1000));
    probe->stall_flush = false;  // the detached writer finishes on the state it owns
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
