#ifndef EXSIM_COMMON_CLOCKS_HPP
#define EXSIM_COMMON_CLOCKS_HPP
#include <cstdint>
#include <ctime>
namespace exsim::common {
inline constexpr std::uint64_t kNanosecondsPerSecond = 1000000000;
// The one ns-per-day constant: trading-day bounds, ITCH timestamps, CME trade dates.
inline constexpr std::uint64_t kNanosecondsPerDay = 86400 * kNanosecondsPerSecond;
inline std::uint64_t monotonic_ns() noexcept {
    timespec time{};
    clock_gettime(CLOCK_MONOTONIC, &time);
    return static_cast<std::uint64_t>(time.tv_sec) * kNanosecondsPerSecond + time.tv_nsec;
}
inline std::uint64_t realtime_ns() noexcept {
    timespec time{};
    clock_gettime(CLOCK_REALTIME, &time);
    return static_cast<std::uint64_t>(time.tv_sec) * kNanosecondsPerSecond + time.tv_nsec;
}
// Cache the UTC offset during setup (and refresh at session/day or timezone
// changes). No libc timezone conversion, locks or lazy allocation per message.
class LocalClock {
public:
    void refresh(std::time_t now = std::time(nullptr)) noexcept {
        std::tm local{};
        localtime_r(&now, &local);
        offset_seconds_ = local.tm_gmtoff;
    }
    std::uint64_t midnight_ns(std::uint64_t unix_ns) const noexcept {
        auto seconds = static_cast<std::int64_t>((unix_ns / kNanosecondsPerSecond) % 86400) + offset_seconds_;
        seconds = (seconds % 86400 + 86400) % 86400;
        return static_cast<std::uint64_t>(seconds) * kNanosecondsPerSecond + unix_ns % kNanosecondsPerSecond;
    }

private:
    std::int64_t offset_seconds_ = 0;
};
}  // namespace exsim::common
#endif
