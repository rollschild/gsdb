#ifndef GSDB_REGISTER_INFO_HPP
#define GSDB_REGISTER_INFO_HPP

#include <cstdint>
#include <string_view>

namespace gsdb {
// This will generate:
// enum class register_id { rax, rdx, rcx, ... };
enum class register_id {
#define DEFINE_REGISTER(name, dwarf_id, size, offset, type, format) name
#include <libgsdb/detail/registers.inc>
#undef DEFINE_REGISTER
};

enum class register_type {
    // general purpose
    gpr,
    // `eax` is the 32-bit version of `rax`
    sub_gpr,
    fpr,
    dr
};

/**
 * Different ways of interpreting a register
 */
enum class register_format { uint, double_float, long_double, vector };

/**
 * All information we need about a single register
 */
struct register_info {
    register_id id;
    std::string_view name;
    std::int32_t dwarf_id;
    std::size_t size;
    std::size_t offset;
    register_type type;
    register_format format;
};

// `inline`: let us define this array in the header so we can deduce the number
// of registers automatically from the initializer
inline constexpr const register_info g_register_infos[] = {
#define DEFINE_REGISTER(name, dwarf_id, size, offset, type, format) \
    {register_id::name, #name, dwarf_id, size, offset, type, format}
#include <libgsdb/detail/registers.inc>
#undef DEFINE_REGISTER
};

}  // namespace gsdb

#endif
