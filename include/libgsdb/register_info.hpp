#ifndef GSDB_REGISTER_INFO_HPP
#define GSDB_REGISTER_INFO_HPP

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string_view>

#include "libgsdb/error.hpp"

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
    // byte offset inside the `user` struct at which the register data live
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

/**
 * Takes a comparator function and uses it to find a specific register info
 * entry
 */
template <class F>
const register_info& register_info_by(F f) {
    auto it = std::find_if(std::begin(g_register_infos),
                           std::end(g_register_infos), f);
    if (it == std::end(g_register_infos)) {
        error::send("CANNOT find register info");
    }

    return *it;
}

// inline allows multitple definitions of one function to exist;
// the linker will throw away all but one of them
inline const register_info& register_info_by_id(register_id id) {
    return register_info_by([id](auto& i) { return i.id == id; });
}
inline const register_info& register_info_by_name(std::string_view name) {
    return register_info_by([name](auto& i) { return i.name == name; });
}
inline const register_info& register_info_by_dwarf(std::int32_t dwarf_id) {
    return register_info_by(
        [dwarf_id](auto& i) { return i.dwarf_id == dwarf_id; });
}

}  // namespace gsdb

#endif
