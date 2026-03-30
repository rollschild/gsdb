#ifndef GSDB_TYPES_HPP
#define GSDB_TYPES_HPP

#include <array>
#include <cstddef>

namespace gsdb {
using byte64 = std::array<std::byte, 8>;
using byte128 = std::array<std::byte, 16>;
}  // namespace gsdb

#endif
