#ifndef EXSIM_BENCH_HISTOGRAM_HPP
#define EXSIM_BENCH_HISTOGRAM_HPP

// Fixed-bucket log-linear histogram of durations in nanoseconds. Memory is
// constant (about 15 KB) and record() is O(1), so percentiles never need a
// sorted copy of the samples. Each power of two is split into 32 buckets, so
// a reported value is within about 3% of the recorded one. Min, max and sum
// are exact.

#include <array>
#include <bit>
#include <cstddef>
#include <cmath>
#include <cstdint>

namespace bench {

class LatencyHistogram {
public:
    void record(std::uint64_t nanoseconds) noexcept {
        ++counts_[bucket_of(nanoseconds)];
        ++count_;
        sum_ += nanoseconds;
        if (nanoseconds < min_) min_ = nanoseconds;
        if (nanoseconds > max_) max_ = nanoseconds;
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t sum() const noexcept { return sum_; }
    [[nodiscard]] std::uint64_t min() const noexcept { return count_ == 0 ? 0 : min_; }
    [[nodiscard]] std::uint64_t max() const noexcept { return max_; }

    // Smallest bucket midpoint such that at least `fraction` of the samples are at or below it.
    [[nodiscard]] std::uint64_t percentile(double fraction) const noexcept {
        if (count_ == 0) return 0;
        const auto wanted = static_cast<std::uint64_t>(std::ceil(fraction * static_cast<double>(count_)));
        std::uint64_t seen = 0;
        for (std::size_t bucket = 0; bucket < counts_.size(); ++bucket) {
            seen += counts_[bucket];
            if (seen >= wanted) return midpoint_of(bucket);
        }
        return max_;
    }

private:
    static constexpr unsigned kSubBucketBits = 5;
    static constexpr std::uint64_t kSubBuckets = std::uint64_t{1} << kSubBucketBits;
    static constexpr std::size_t kBucketCount = (64 - kSubBucketBits + 1) * kSubBuckets;

    static std::size_t bucket_of(std::uint64_t value) noexcept {
        if (value < kSubBuckets) return static_cast<std::size_t>(value);
        const unsigned shift = static_cast<unsigned>(std::bit_width(value)) - 1 - kSubBucketBits;
        return static_cast<std::size_t>(((shift + 1) << kSubBucketBits) + (value >> shift) - kSubBuckets);
    }

    static std::uint64_t midpoint_of(std::size_t bucket) noexcept {
        if (bucket < kSubBuckets) return bucket;
        const unsigned shift = static_cast<unsigned>(bucket >> kSubBucketBits) - 1;
        const std::uint64_t lower = ((bucket & (kSubBuckets - 1)) + kSubBuckets) << shift;
        return lower + ((std::uint64_t{1} << shift) >> 1);
    }

    std::array<std::uint64_t, kBucketCount> counts_{};
    std::uint64_t count_ = 0;
    std::uint64_t sum_ = 0;
    std::uint64_t min_ = UINT64_MAX;
    std::uint64_t max_ = 0;
};

}  // namespace bench

#endif
