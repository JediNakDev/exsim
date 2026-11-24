#ifndef EXSIM_COMMON_PRICE_TEXT_HPP
#define EXSIM_COMMON_PRICE_TEXT_HPP

// The one fixed-point Price (4 implied decimals) to decimal text formatter.
// Trailing fraction zeros are trimmed down to `minimum_decimals` (at most 4):
//
//   format_price(70'000, 0) == "7"        FIX wire and config display
//   format_price(70'000, 2) == "7.00"     test diagnostics
//   format_price(1'002'500, 0) == "100.25"

#include <common/types.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace exsim {

// Signed decimal with up to four fraction digits. The integer part may be
// omitted, but a decimal point must be followed by at least one digit.
inline bool parse_decimal_price(std::string_view text, Price& out) noexcept {
    if (text.empty()) {
        return false;
    }
    bool negative = false;
    if (text.front() == '-') {
        negative = true;
        text.remove_prefix(1);
    }
    const std::size_t dot = text.find('.');
    const std::string_view whole = text.substr(0, dot);
    const std::string_view frac = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    if ((whole.empty() && frac.empty()) || frac.size() > 4 || (dot != std::string_view::npos && frac.empty())) {
        return false;
    }
    constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<Price>::max());
    const std::uint64_t limit = negative ? maximum + 1 : maximum;
    std::uint64_t value = 0;
    for (char c : whole) {
        if (c < '0' || c > '9') {
            return false;
        }
        if (value > (limit - static_cast<unsigned>(c - '0')) / 10) {
            return false;
        }
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    if (value > limit / kPriceScale) {
        return false;
    }
    value *= kPriceScale;
    std::uint64_t scale = kPriceScale / 10;
    for (char c : frac) {
        if (c < '0' || c > '9') {
            return false;
        }
        const auto fraction_units = static_cast<unsigned>(c - '0') * scale;
        if (value > limit - fraction_units) return false;
        value += fraction_units;
        scale /= 10;
    }
    out = negative && value == maximum + 1 ? std::numeric_limits<Price>::min()
          : negative                       ? -static_cast<Price>(value)
                                           : static_cast<Price>(value);
    return true;
}

// Writes at most `capacity` bytes and returns the length, or returns zero
// without writing when the text does not fit. Allocation-free.
[[nodiscard]] inline std::size_t format_price(Price price, char* output, std::size_t capacity,
                                              unsigned minimum_decimals) noexcept {
    char text[32];
    char* end = text;
    const bool negative = price < 0;
    // Unsigned negation is well defined for every int64 value, including INT64_MIN.
    const std::uint64_t magnitude =
        negative ? std::uint64_t{0} - static_cast<std::uint64_t>(price) : static_cast<std::uint64_t>(price);
    const auto scale = static_cast<std::uint64_t>(kPriceScale);
    if (negative) *end++ = '-';
    end = std::to_chars(end, text + sizeof text, magnitude / scale).ptr;
    const std::uint64_t fraction = magnitude % scale;
    minimum_decimals = std::min(minimum_decimals, 4u);
    if (fraction != 0 || minimum_decimals != 0) {
        *end++ = '.';
        char* const first_decimal = end;
        for (std::uint64_t divisor = scale / 10; divisor != 0; divisor /= 10) {
            *end++ = static_cast<char>('0' + fraction / divisor % 10);
        }
        while (end > first_decimal + minimum_decimals && end[-1] == '0') --end;
    }
    const auto size = static_cast<std::size_t>(end - text);
    if (size > capacity) return 0;
    std::memcpy(output, text, size);
    return size;
}

[[nodiscard]] inline std::string format_price(Price price, unsigned minimum_decimals) {
    char text[32];
    return std::string(text, format_price(price, text, sizeof text, minimum_decimals));
}

}  // namespace exsim
#endif
