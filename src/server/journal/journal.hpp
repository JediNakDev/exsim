#ifndef EXSIM_SERVER_JOURNAL_JOURNAL_HPP
#define EXSIM_SERVER_JOURNAL_JOURNAL_HPP
#include <server/journal/canonical.hpp>
#include <engine/command.hpp>
#include <engine/instrument.hpp>
#include <engine/matching_engine.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace exsim::server::journal {
struct JournalSetup {
    EngineConfig engine;
    std::vector<InstrumentConfig> instruments;
};
// Sequence, size, command, output digest, checksum: 32 bytes around a payload of at most 104.
inline constexpr std::size_t kMaxRecordBytes = 136;
class JournalWriter {
public:
    JournalWriter(const std::string& path, const JournalSetup& setup);
    ~JournalWriter();
    JournalWriter(const JournalWriter&) = delete;
    JournalWriter& operator=(const JournalWriter&) = delete;
    // `output` is the output_digest of the events the command produced.
    [[nodiscard]] bool append(const Command& command, std::uint64_t output) noexcept;  // no allocation, incl. flush
    // Writes buffered records and a checkpoint to the OS page cache only.
    // No fsync/fdatasync: success does not guarantee survival of power loss.
    [[nodiscard]] bool flush() noexcept;

private:
    int fd_ = -1;
    std::array<unsigned char, 128 * kMaxRecordBytes> buffer_{};
    std::size_t used_ = 0;
    std::uint64_t sequence_ = 0;
    bool footer_ = false;
    bool failed_ = false;
};
struct JournalRecord {
    Command command;
    std::uint64_t output = 0;  // output_digest of the events the command produced
};
// Reads a journal one record at a time, so memory does not grow with its length. The
// constructor checks the header and the footer; next() checks each record's length,
// sequence number, checksum and canonical encoding, then the footer's record count.
// Both throw std::runtime_error naming the first problem and where it is.
class JournalReader {
public:
    explicit JournalReader(const std::string& path);
    [[nodiscard]] const JournalSetup& setup() const noexcept { return setup_; }
    // The next record, or nullopt after the last one.
    [[nodiscard]] std::optional<JournalRecord> next();

private:
    struct Close {
        void operator()(std::FILE* file) const noexcept { std::fclose(file); }
    };
    std::unique_ptr<std::FILE, Close> file_;
    JournalSetup setup_;
    std::uint64_t offset_ = 0;    // where the next record starts
    std::uint64_t tail_ = 0;      // where the footer starts
    std::uint64_t count_ = 0;     // records the footer counts
    std::uint64_t sequence_ = 0;  // of the next record
};
}  // namespace exsim::server::journal
#endif
