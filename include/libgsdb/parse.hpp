#ifndef GSDB_PARSE_HPP
#define GSDB_PARSE_HPP

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "libgsdb/error.hpp"

namespace gsdb {
template <class I>
std::optional<I> to_integral(std::string_view sv, int base = 10) {
    auto begin = sv.begin();
    if (base == 16 and sv.size() > 1 and begin[0] == '0' and begin[1] == 'x') {
        begin += 2;
    }

    I ret{};
    // from_chars will succeed even if the entire string has _not_ been read
    auto result = std::from_chars(begin, sv.end(), ret, base);

    /* struct from_chars_result {
        const char* ptr; // points to first char not parsed
        std::errc ec;
    }; */
    if (result.ptr != sv.end()) {
        return std::nullopt;
    }

    return ret;
}

/**
 * Specialization - no longer a template
 * Will be called _only if_ `std::byte` is specified as the template argument.
 */
template <>
inline std::optional<std::byte> to_integral<std::byte>(std::string_view sv,
                                                       int base) {
    auto uint8 = to_integral<std::uint8_t>(sv, base);
    if (uint8) {
        return static_cast<std::byte>(*uint8);
    }
    return std::nullopt;
}

template <class F>
std::optional<F> to_float(std::string_view sv) {
    F ret;
    auto result = std::from_chars(sv.begin(), sv.end(), ret);

    if (result.ptr != sv.end()) {
        return std::nullopt;
    }

    return ret;
}

// template takes compile-time integer
template <std::size_t N>
auto parse_vector(std::string_view text) {
    auto invalid = [] { gsdb::error::send("Invalid format!"); };
    std::array<std::byte, N> bytes;
    const char* c = text.data();
    if (*c++ != '[') invalid();

    for (std::size_t i = 0; i < N - 1; ++i) {
        // parse 4 chars from `c` into a byte
        // because each byte is represented as 4-char hex string like `0xff`
        // so 4 characters per byte
        bytes[i] = to_integral<std::byte>({c, 4}, 16).value();
        c += 4;
        if (*c++ != ',') invalid();
    }

    // parse the last value
    bytes[N - 1] = to_integral<std::byte>({c, 4}, 16).value();
    c += 4;

    if (*c++ != ']') invalid();
    if (c != text.end()) invalid();

    return bytes;
}

/**
 * [0xff,0xff]
 */
inline auto parse_vector(std::string_view text) {
    auto invalid = [] { gsdb::error::send("Invalid text format!"); };

    std::vector<std::byte> bytes;
    const char* c = text.data();
    if (*c++ != '[') invalid();

    while (*c != ']') {
        auto byte = gsdb::to_integral<std::byte>({c, 4}, 16);
        bytes.push_back(byte.value());
        c += 4;  // parse 4 characters, like 0xff

        // ensure we have either a comma or ']'
        if (*c == ',')
            ++c;
        else if (*c != ']')
            invalid();
    }

    if (++c != text.end()) invalid();

    return bytes;
}
}  // namespace gsdb

#endif
