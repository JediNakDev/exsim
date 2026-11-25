#ifndef EXSIM_NET_STREAM_BUFFERS_HPP
#define EXSIM_NET_STREAM_BUFFERS_HPP

// Fixed-capacity byte buffers for the two directions of a TCP connection.
// Storage is allocated once in the constructor and never grows; when a buffer
// runs out of room the caller is told and decides (the TCP server disconnects
// the connection). Bytes are `char`, matching the protocol codecs.
//
// Storage is deliberately not prefaulted. Capacity is sized for the largest
// single write (an MDP replay of about 2.8 MB, an iLink retransmission of
// about 1.25 MB), so prefaulting every slot would make hundreds of MiB
// resident at startup. A page faults once, the first time a burst reaches
// it; the slot keeps the buffer across connections and compaction keeps
// steady traffic in the first pages.
//
//   inbound:  receive_from(fd) appends at the tail -> framer reads readable()
//             and consume()s whole messages -> unread partial bytes are moved
//             to the front only when the tail runs short (no allocation).
//   outbound: codec encodes via reserve()/commit() -> flush_to(fd)
//             sends what the socket accepts; the rest stays queued until the
//             socket is writable again.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <net/socket.hpp>
#include <memory>
#include <span>

namespace exsim::net {

enum class ReceiveStatus : std::uint8_t {
    Received,    // `bytes` > 0 were appended
    WouldBlock,  // nothing to read right now
    PeerClosed,  // orderly shutdown by the peer (recv returned 0)
    BufferFull,  // no free space: consume() before receiving more
    Error,       // `error` holds the errno (ECONNRESET, ...)
};

struct ReceiveResult {
    ReceiveStatus status;
    ErrorCode error;
    std::size_t bytes;
};

class ReceiveBuffer {
public:
    explicit ReceiveBuffer(std::size_t capacity) : storage_(new char[capacity]), capacity_(capacity) {}

    // Bytes received and not yet consumed.
    std::span<const char> readable() const noexcept {
        return {storage_.get() + read_offset_, write_offset_ - read_offset_};
    }
    std::size_t size() const noexcept { return write_offset_ - read_offset_; }
    std::size_t capacity() const noexcept { return capacity_; }

    // Drops `bytes` from the front of readable() (a framer consumes whole messages).
    void consume(std::size_t bytes) noexcept {
        assert(bytes <= size());
        read_offset_ += bytes;
        if (read_offset_ == write_offset_) {
            read_offset_ = 0;
            write_offset_ = 0;
        }
    }

    // Free space after the unread bytes. When less than half the capacity is
    // free at the tail, the unread bytes are first moved to the front.
    std::span<char> writable_tail() noexcept {
        if (read_offset_ != 0 && (capacity_ - write_offset_ < capacity_ / 2 || write_offset_ == capacity_)) {
            const std::size_t unread = write_offset_ - read_offset_;
            std::memmove(storage_.get(), storage_.get() + read_offset_, unread);
            read_offset_ = 0;
            write_offset_ = unread;
        }
        return {storage_.get() + write_offset_, capacity_ - write_offset_};
    }

    // Marks `bytes` written into writable_tail() as received.
    void commit(std::size_t bytes) noexcept {
        assert(bytes <= capacity_ - write_offset_);
        write_offset_ += bytes;
    }

    void clear() noexcept {
        read_offset_ = 0;
        write_offset_ = 0;
    }

    // One recv() into writable_tail(). Never calls recv with zero space (that
    // would be indistinguishable from EOF): returns BufferFull instead.
    ReceiveResult receive_from(int fd) noexcept;

private:
    std::unique_ptr<char[]> storage_;
    std::size_t capacity_;
    std::size_t read_offset_ = 0;
    std::size_t write_offset_ = 0;
};

enum class FlushStatus : std::uint8_t {
    Complete,  // nothing left pending
    Blocked,   // the socket buffer is full and bytes remain: wait for write readiness
    Error,     // `error` holds the errno (EPIPE, ECONNRESET, ...)
};

struct FlushResult {
    FlushStatus status;
    ErrorCode error;
    std::size_t bytes_sent;
};

class SendBuffer {
public:
    explicit SendBuffer(std::size_t capacity) : storage_(new char[capacity]), capacity_(capacity) {}

    // Zero-copy append: returns exactly `max_bytes` of contiguous space at the
    // end of the queue; encode into it, then commit() the bytes actually used.
    // Returns an empty span when the bytes do not fit, without moving the
    // queued bytes: a paused replay retries until enough is sent.
    std::span<char> reserve(std::size_t max_bytes) noexcept {
        if (capacity_ - write_offset_ < max_bytes) {
            if (capacity_ - (write_offset_ - read_offset_) < max_bytes) return {};
            compact();
        }
        return {storage_.get() + write_offset_, max_bytes};
    }

    void commit(std::size_t bytes) noexcept {
        assert(bytes <= capacity_ - write_offset_);
        write_offset_ += bytes;
    }

    std::span<const char> pending() const noexcept {
        return {storage_.get() + read_offset_, write_offset_ - read_offset_};
    }
    bool has_pending() const noexcept { return write_offset_ != read_offset_; }

    // One send() of the pending bytes. A short send means the socket buffer
    // is full, so the rest waits for write readiness (status Blocked).
    FlushResult flush_to(int fd) noexcept;

    void clear() noexcept {
        read_offset_ = 0;
        write_offset_ = 0;
    }

private:
    void compact() noexcept {
        if (read_offset_ == 0) {
            return;
        }
        const std::size_t pending_count = write_offset_ - read_offset_;
        std::memmove(storage_.get(), storage_.get() + read_offset_, pending_count);
        read_offset_ = 0;
        write_offset_ = pending_count;
    }

    std::unique_ptr<char[]> storage_;
    std::size_t capacity_;
    std::size_t read_offset_ = 0;
    std::size_t write_offset_ = 0;
};

}  // namespace exsim::net

#endif
