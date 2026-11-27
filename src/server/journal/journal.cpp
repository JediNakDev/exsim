#include <server/journal/journal.hpp>
#include <algorithm>
#include <bit>
#include <cerrno>
#include <fcntl.h>
#include <cstdio>
#include <cstring>
#include <limits>
#include <unistd.h>
#include <system_error>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
namespace exsim::server::journal {
namespace {
constexpr std::uint64_t kHeaderMagic = 0x455853494d4a4e4cull;  // "EXSIMJNL"
constexpr std::uint64_t kFooterMagic = 0x455853494d454e44ull;  // "EXSIMEND"
constexpr std::size_t kInstrumentBytes = 8 + 20 + 10 * 8;      // symbol, CME symbol, ten words
bool write_all(int fd, const unsigned char* p, std::size_t n) noexcept {
    while (n) {
        auto k = ::write(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return false;
        p += k;
        n -= static_cast<std::size_t>(k);
    }
    return true;
}
void add(std::vector<unsigned char>& b, std::uint64_t v) {
    auto n = b.size();
    b.resize(n + 8);
    store64(b.data() + n, v);
}
std::optional<Command> decode_command(std::span<const unsigned char, 264> bytes) {
    std::size_t offset = 8;
    auto get = [&]() {
        auto v = load64(bytes.data() + offset);
        offset += 8;
        return v;
    };
    switch (static_cast<CommandType>(load64(bytes.data()))) {
        case CommandType::NewOrder: {
            NewOrder x;
            x.ts = static_cast<decltype(x.ts)>(get());
            x.instrument = static_cast<decltype(x.instrument)>(get());
            x.session = static_cast<decltype(x.session)>(get());
            x.owner = static_cast<decltype(x.owner)>(get());
            x.cl_ord_id = static_cast<decltype(x.cl_ord_id)>(get());
            x.side = static_cast<decltype(x.side)>(get());
            x.type = static_cast<decltype(x.type)>(get());
            x.tif = static_cast<decltype(x.tif)>(get());
            x.stp = static_cast<decltype(x.stp)>(get());
            x.post_only = static_cast<decltype(x.post_only)>(get());
            x.price = std::bit_cast<Price>(get());
            x.qty = static_cast<decltype(x.qty)>(get());
            return Command{x};
        }
        case CommandType::Cancel: {
            CancelOrder x;
            x.ts = static_cast<decltype(x.ts)>(get());
            x.session = static_cast<decltype(x.session)>(get());
            x.orig_cl_ord_id = static_cast<decltype(x.orig_cl_ord_id)>(get());
            x.cl_ord_id = static_cast<decltype(x.cl_ord_id)>(get());
            x.reduce_to_total_qty = static_cast<decltype(x.reduce_to_total_qty)>(get());
            return Command{x};
        }
        case CommandType::Replace: {
            ReplaceOrder x;
            x.ts = static_cast<decltype(x.ts)>(get());
            x.session = static_cast<decltype(x.session)>(get());
            x.orig_cl_ord_id = static_cast<decltype(x.orig_cl_ord_id)>(get());
            x.cl_ord_id = static_cast<decltype(x.cl_ord_id)>(get());
            x.price = std::bit_cast<Price>(get());
            x.qty = static_cast<decltype(x.qty)>(get());
            x.force_priority_loss = static_cast<decltype(x.force_priority_loss)>(get());
            return Command{x};
        }
        case CommandType::MassCancel: {
            MassCancel x;
            x.ts = static_cast<decltype(x.ts)>(get());
            x.session = static_cast<decltype(x.session)>(get());
            x.instrument = static_cast<decltype(x.instrument)>(get());
            x.cl_ord_id = static_cast<decltype(x.cl_ord_id)>(get());
            x.cancel_on_disconnect = static_cast<decltype(x.cancel_on_disconnect)>(get());
            return Command{x};
        }
        case CommandType::SetPhase: {
            SetPhase x;
            x.ts = static_cast<decltype(x.ts)>(get());
            x.instrument = static_cast<decltype(x.instrument)>(get());
            x.phase = static_cast<decltype(x.phase)>(get());
            return Command{x};
        }
    }
    return std::nullopt;
}
}  // namespace
JournalWriter::JournalWriter(const std::string& path, const JournalSetup& setup) {
    (void)required_event_capacity(setup.engine);
    if (setup.instruments.size() > setup.engine.max_instruments) throw std::invalid_argument("instrument count");
    std::vector<unsigned char> header;
    add(header, kHeaderMagic);
    add(header, kSchema);
    add(header, setup.engine.max_instruments);
    add(header, setup.engine.max_orders);
    add(header, setup.engine.max_level_qty);
    add(header, setup.instruments.size());
    for (const auto& i : setup.instruments) {
        if (!valid_instrument_config(i)) throw std::invalid_argument("instrument config");
        for (char c : i.symbol.chars) header.push_back(static_cast<unsigned char>(c));
        add(header, static_cast<std::uint64_t>(i.tick_size));
        add(header, i.lot_size);
        add(header, i.max_order_qty);
        add(header, static_cast<std::uint64_t>(i.min_price));
        add(header, static_cast<std::uint64_t>(i.max_price));
        add(header, static_cast<std::uint64_t>(i.reference_price));
        add(header, static_cast<unsigned>(i.initial_phase));
        add(header, static_cast<std::uint32_t>(i.cme_security_id));
        for (char c : i.cme_symbol) header.push_back(static_cast<unsigned char>(c));
        add(header, i.cme_maturity_date);
        add(header, i.cme_contract_size);
    }
    add(header, hash_bytes(header));
    // Nonblocking open avoids waiting for a FIFO reader. O_NOFOLLOW refuses a symlink as the
    // file itself. Validate the descriptor and take an exclusive lock before truncating, so special
    // files are never used and a second writer cannot erase a live journal.
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd_ < 0) throw std::system_error(errno, std::generic_category(), "journal open");
    try {
        struct stat info {};
        if (::fstat(fd_, &info) != 0) throw std::system_error(errno, std::generic_category(), "journal stat");
        if (!S_ISREG(info.st_mode)) throw std::invalid_argument("journal destination must be a regular file");
        if (::flock(fd_, LOCK_EX | LOCK_NB) != 0)
            throw std::system_error(errno, std::generic_category(), "journal is in use by another writer");
        if (::ftruncate(fd_, 0) != 0 || ::lseek(fd_, 0, SEEK_SET) < 0)
            throw std::system_error(errno, std::generic_category(), "journal truncate");
        if (!write_all(fd_, header.data(), header.size()))
            throw std::system_error(errno, std::generic_category(), "journal header");
    } catch (...) {
        ::close(fd_);
        fd_ = -1;
        throw;
    }
}
JournalWriter::~JournalWriter() {
    if (fd_ >= 0) ::close(fd_);
}
// Explicit flush required: destructor cannot report I/O failures.
bool JournalWriter::flush() noexcept {
    if (failed_) return false;
    if (footer_) return true;
    if (!write_all(fd_, buffer_.data(), used_)) {
        failed_ = true;
        return false;
    }
    used_ = 0;
    unsigned char footer[24]{};
    store64(footer, kFooterMagic);
    store64(footer + 8, sequence_);
    store64(footer + 16, hash_bytes({footer, 16}));
    if (!write_all(fd_, footer, 24)) {
        failed_ = true;
        return false;
    }
    footer_ = true;
    return true;
}
bool JournalWriter::append(const Command& c, std::uint64_t output) noexcept {
    if (failed_) return false;
    if (used_ + kMaxRecordBytes > buffer_.size() && !flush()) return false;
    if (footer_) {
        if (::lseek(fd_, -24, SEEK_CUR) < 0) {
            failed_ = true;
            return false;
        }
        footer_ = false;
    }
    auto* p = buffer_.data() + used_;
    store64(p, sequence_);
    store64(p + 16, static_cast<std::uint64_t>(c.type));
    std::size_t size = 8;
    visit_fields(c, [&](const Field& field) {
        store64(p + 16 + size, field.raw);
        size += 8;
    });
    store64(p + 8, size);
    store64(p + 16 + size, output);
    store64(p + 24 + size, hash_bytes({p, 24 + size}));
    used_ += size + 32;
    ++sequence_;
    return true;
}
JournalReader::JournalReader(const std::string& path) {
    // A nonblocking open, so a FIFO cannot wait for a writer, and only a regular file, checked
    // on the descriptor itself.
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error(std::string("cannot open: ") + std::strerror(errno));
    file_.reset(::fdopen(fd, "rb"));
    if (!file_) {
        ::close(fd);
        throw std::runtime_error("cannot open");
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) throw std::runtime_error("cannot open: not a regular file");
    const auto size = static_cast<std::uint64_t>(info.st_size);
    auto read = [this](unsigned char* out, std::size_t n) {
        if (std::fread(out, 1, n, file_.get()) != n) throw std::runtime_error("cannot read");
    };
    auto seek = [this](std::uint64_t offset) {
        if (::fseeko(file_.get(), static_cast<off_t>(offset), SEEK_SET) != 0) throw std::runtime_error("cannot read");
    };
    // Header: magic, schema, three engine limits, instrument count, instruments, checksum.
    if (size < 56) throw std::runtime_error("truncated header");
    std::vector<unsigned char> header(48);
    read(header.data(), 48);
    if (load64(header.data()) != kHeaderMagic) throw std::runtime_error("not a journal");
    if (load64(header.data() + 8) != kSchema)
        throw std::runtime_error("schema " + std::to_string(load64(header.data() + 8)) + ", this build reads " +
                                 std::to_string(kSchema));
    const auto count = load64(header.data() + 40);
    if (count > (size - 56) / kInstrumentBytes) throw std::runtime_error("truncated header");
    if (count > std::numeric_limits<InstrumentId>::max()) throw std::runtime_error("instrument count");
    const std::uint64_t instruments_end = 48 + count * kInstrumentBytes;
    header.resize(instruments_end + 8);
    read(header.data() + 48, header.size() - 48);
    if (load64(header.data() + instruments_end) != hash_bytes({header.data(), instruments_end}))
        throw std::runtime_error("header checksum");
    setup_.engine.max_instruments = load64(header.data() + 16);
    setup_.engine.max_orders = load64(header.data() + 24);
    setup_.engine.max_level_qty = load64(header.data() + 32);
    (void)required_event_capacity(setup_.engine);
    if (count > setup_.engine.max_instruments) throw std::runtime_error("instrument count");
    const unsigned char* p = header.data() + 48;
    auto word = [&p] {
        const auto v = load64(p);
        p += 8;
        return v;
    };
    auto text = [&p](auto& chars) {
        for (auto& c : chars) c = static_cast<char>(*p++);
    };
    for (std::uint64_t n = 0; n < count; ++n) {
        InstrumentConfig i;
        text(i.symbol.chars);
        i.tick_size = std::bit_cast<Price>(word());
        const auto lot = word(), qty = word();
        i.min_price = std::bit_cast<Price>(word());
        i.max_price = std::bit_cast<Price>(word());
        i.reference_price = std::bit_cast<Price>(word());
        const auto phase = word();
        const auto security_id = word();
        text(i.cme_symbol);
        const auto maturity = word(), contract_size = word();
        // The writer zero-extends these 32-bit fields; a wider word is damage, not data.
        if (std::max({lot, qty, security_id, maturity, contract_size}) > UINT32_MAX)
            throw std::runtime_error("instrument field width");
        if (phase > static_cast<unsigned>(Phase::PreClose)) throw std::runtime_error("instrument phase");
        i.lot_size = static_cast<Quantity>(lot);
        i.max_order_qty = static_cast<Quantity>(qty);
        i.initial_phase = static_cast<Phase>(phase);
        i.cme_security_id = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(security_id));
        i.cme_maturity_date = static_cast<std::uint32_t>(maturity);
        i.cme_contract_size = static_cast<std::uint32_t>(contract_size);
        // Duplicate symbols are left to the engine: add_instrument refuses them by hash lookup.
        if (!valid_instrument_config(i)) throw std::runtime_error("instrument config");
        setup_.instruments.push_back(i);
    }
    offset_ = instruments_end + 8;
    // Footer: the checkpoint the last flush wrote. Without it the file was cut short.
    if (size - offset_ < 24) throw std::runtime_error("truncated: no footer");
    tail_ = size - 24;
    unsigned char footer[24];
    seek(tail_);
    read(footer, 24);
    if (load64(footer) != kFooterMagic || load64(footer + 16) != hash_bytes({footer, 16}))
        throw std::runtime_error("truncated or damaged: no valid footer at byte " + std::to_string(tail_));
    count_ = load64(footer + 8);
    seek(offset_);
}
std::optional<JournalRecord> JournalReader::next() {
    if (offset_ >= tail_) {
        if (count_ != sequence_)
            throw std::runtime_error("footer counts " + std::to_string(count_) + " records, found " +
                                     std::to_string(sequence_));
        return std::nullopt;
    }
    auto fail = [this](const std::string& what) {
        return std::runtime_error(what + " in record " + std::to_string(sequence_) + " at byte " +
                                  std::to_string(offset_));
    };
    std::array<unsigned char, kMaxRecordBytes> record{};
    if (tail_ - offset_ < 32) throw fail("truncated record");
    if (std::fread(record.data(), 1, 16, file_.get()) != 16) throw fail("cannot read");
    const auto length = load64(record.data() + 8);
    if (length < 32 || length > 104 || length % 8 != 0 || length > tail_ - offset_ - 32)
        throw fail("bad length " + std::to_string(length));
    if (std::fread(record.data() + 16, 1, length + 16, file_.get()) != length + 16) throw fail("cannot read");
    if (load64(record.data()) != sequence_) throw fail("sequence number " + std::to_string(load64(record.data())));
    if (load64(record.data() + 24 + length) != hash_bytes({record.data(), 24 + length})) throw fail("bad checksum");
    std::array<unsigned char, 264> payload{};
    std::copy_n(record.data() + 16, length, payload.begin());
    const auto command = decode_command(payload);
    if (!command) throw fail("unknown command type");
    std::size_t fields = 0;
    visit_fields(*command, [&fields](const Field&) { ++fields; });
    if (length != 8 + 8 * fields || encode(*command) != payload) throw fail("noncanonical command");
    offset_ += length + 32;
    ++sequence_;
    return JournalRecord{*command, load64(record.data() + 16 + length)};
}
}  // namespace exsim::server::journal
