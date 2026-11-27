#ifndef EXSIM_SERVER_RUNTIME_JOURNAL_WORKER_HPP
#define EXSIM_SERVER_RUNTIME_JOURNAL_WORKER_HPP

#include "server/journal/journal.hpp"
#include "util/spsc_queue.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace exsim::server::runtime {

// The reactor only enqueues commands and flush requests. The writer policy
// owns all file operations after startup. Accepted entries are never dropped
// silently: overflow or an I/O error latches failure for the exchange core.
// A stalled writer retains its own state if shutdown must abandon the wait.
// The default ring holds 32767 entries.
template <class Writer = journal::JournalWriter, std::size_t Capacity = 32768>
class JournalWorker {
public:
    // Tests pass a long shutdown_wait so a writer starved of CPU is not abandoned.
    JournalWorker(const std::string& path, const journal::JournalSetup& setup,
                  std::chrono::milliseconds shutdown_wait = std::chrono::milliseconds(100))
        : state_(std::make_shared<State>(path, setup)),
          shutdown_wait_(shutdown_wait),
          worker_([state = state_] { persist(*state); }) {}

    ~JournalWorker() {
        state_->stopping.store(true, std::memory_order_release);
        state_->wake.fetch_add(1, std::memory_order_release);
        state_->wake.notify_one();
        // The writer drains, closes and signals completion; wait up to shutdown_wait_.
        std::unique_lock lock(state_->completion_mutex);
        const bool writer_done = state_->completion.wait_for(lock, shutdown_wait_, [&] { return state_->done; });
        lock.unlock();
        if (writer_done)
            worker_.join();
        else {
            abort();
            // The worker captures the state; it never accesses the runtime.
            // Process exit can reclaim a writer stuck in the kernel.
            worker_.detach();
        }
    }

    JournalWorker(const JournalWorker&) = delete;
    JournalWorker& operator=(const JournalWorker&) = delete;

    [[nodiscard]] bool append(const Command& command, std::uint64_t output) noexcept {
        if (!enqueue({command, output, false})) return false;
        last_operation_flush_ = false;
        return true;
    }
    // Accepts a request to write to the page cache; drained() acknowledges it.
    // Neither operation guarantees persistence across power loss.
    [[nodiscard]] bool flush() noexcept {
        if (!healthy()) return false;
        if (last_operation_flush_) return true;
        if (!enqueue({{}, 0, true})) return false;
        last_operation_flush_ = true;
        return true;
    }
    [[nodiscard]] bool healthy() const noexcept { return !state_->failed.load(std::memory_order_acquire); }
    [[nodiscard]] bool drained() const noexcept {
        return state_->flushed_operations.load(std::memory_order_acquire) == submitted_operations_;
    }
    void abort() noexcept {
        state_->failed.store(true, std::memory_order_release);
        state_->stopping.store(true, std::memory_order_release);
        state_->wake.fetch_add(1, std::memory_order_release);
        state_->wake.notify_one();
    }

private:
    struct Entry {
        Command command;
        std::uint64_t output;
        bool flush;
    };
    struct State {
        State(const std::string& path, const journal::JournalSetup& setup) : writer(std::in_place, path, setup) {}
        std::optional<Writer> writer;  // closed by the persistence thread
        exsim::util::SPSCQueue<Entry, Capacity> queue;
        std::atomic<std::uint64_t> wake{0};
        std::atomic<bool> stopping{false};
        std::atomic<bool> failed{false};
        std::mutex completion_mutex;
        std::condition_variable completion;
        bool done = false;
        std::atomic<std::uint64_t> flushed_operations{0};
    };

    bool enqueue(const Entry& entry) noexcept {
        if (!healthy() || state_->stopping.load(std::memory_order_acquire)) return false;
        if (!state_->queue.try_push(entry)) {
            abort();
            return false;
        }
        ++submitted_operations_;
        state_->wake.fetch_add(1, std::memory_order_release);
        state_->wake.notify_one();
        return true;
    }

    static void persist(State& state) noexcept {
        std::uint64_t processed = 0;
        bool io_ok = true;
        for (;;) {
            const auto wake = state.wake.load(std::memory_order_acquire);
            Entry entry{};
            if (state.queue.try_pop(entry)) {
                ++processed;
                io_ok = entry.flush ? state.writer->flush() : state.writer->append(entry.command, entry.output);
                if (!io_ok) break;
                if (entry.flush) state.flushed_operations.store(processed, std::memory_order_release);
            } else if (state.stopping.load(std::memory_order_acquire)) {
                // Recheck after observing stopping so an entry published
                // before it cannot be missed by the final drain.
                if (!state.queue.empty()) continue;
                io_ok = state.writer->flush();
                if (io_ok) state.flushed_operations.store(processed, std::memory_order_release);
                break;
            } else {
                state.wake.wait(wake, std::memory_order_acquire);
            }
        }
        if (!io_ok) state.failed.store(true, std::memory_order_release);
        state.writer.reset();  // close can block too; keep it off the reactor
        {
            std::lock_guard lock(state.completion_mutex);
            state.done = true;
        }
        state.completion.notify_one();
    }

    std::shared_ptr<State> state_;
    std::chrono::milliseconds shutdown_wait_;
    std::uint64_t submitted_operations_ = 0;  // reactor-owned
    bool last_operation_flush_ = false;
    // jthread cannot interrupt file I/O; retain bounded shutdown and detach.
    std::thread worker_;
};

}  // namespace exsim::server::runtime

#endif
