#ifndef GSDB_BIT_HPP
#define GSDB_BIT_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <libgsdb/types.hpp>
#include <string_view>
#include <vector>

namespace gsdb {
inline std::string_view to_string_view(const std::byte* data,
                                       std::size_t size) {
    return {reinterpret_cast<const char*>(data), size};
}

inline std::string_view to_string_view(const std::vector<std::byte>& data) {
    return to_string_view(data.data(), data.size());
}

template <class To>
To from_bytes(const std::byte* bytes) {
    To ret;
    std::memcpy(&ret, bytes, sizeof(To));
    return ret;
}

template <class From>
std::byte* as_bytes(From& from) {
    return reinterpret_cast<std::byte*>(&from);
}

template <class From>
const std::byte* as_bytes(const From& from) {
    return reinterpret_cast<const std::byte*>(&from);
}

template <typename From>
byte128 to_byte128(From src) {
    byte128 ret{};
    std::memcpy(&ret, &src, sizeof(From));
    return ret;
}

template <typename From>
byte64 to_byte64(From src) {
    byte64 ret{};
    std::memcpy(&ret, &src, sizeof(From));
    return ret;
}

/**
 * Handle copying data that isn't aligned to a byte
 */
inline void memcpy_bits(std::uint8_t* dest, std::uint32_t dest_bit,
                        const std::uint8_t* src, std::uint32_t src_bit,
                        std::uint32_t n_bits) {
    // copy data one bit at a time
    for (; n_bits; --n_bits, ++src_bit, ++dest_bit) {
        // clear that bit in the destination data
        std::uint8_t dest_mask = 1 << (dest_bit % 8);
        // dest_bit / 8, the byte selector
        dest[dest_bit / 8] &=
            ~dest_mask;  // ~dest_mask: `0b0000'1000` becomes `0b1111'0111`
        auto src_mask = 1 << (src_bit % 8);
        auto corresponding_src_bit_set = src[src_bit / 8] & src_mask;
        if (corresponding_src_bit_set) {
            dest[dest_bit / 8] |= dest_mask;
        }
    }
}

}  // namespace gsdb

#endif
