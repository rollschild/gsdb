#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <iterator>
#include <libgsdb/bit.hpp>
#include <libgsdb/process.hpp>
#include <libgsdb/registers.hpp>
#include <type_traits>
#include <variant>

#include "libgsdb/error.hpp"
#include "libgsdb/register_info.hpp"
#include "libgsdb/types.hpp"

namespace {
template <class T>
gsdb::byte128 widen(const gsdb::register_info& info, T t) {
    using namespace gsdb;

    if constexpr (std::is_floating_point_v<T>) {
        if (info.format == register_format::double_float) {
            return to_byte128(static_cast<double>(t));
        }
        if (info.format == register_format::long_double) {
            return to_byte128(static_cast<long double>(t));
        }
    } else if constexpr (std::is_signed_v<T>) {
        if (info.format == register_format::uint) {
            switch (info.size) {
                case 2:
                    return to_byte128(static_cast<std::int16_t>(t));
                case 4:
                    return to_byte128(static_cast<std::int32_t>(t));
                case 8:
                    return to_byte128(static_cast<std::int64_t>(t));
            }
        }
    }

    return to_byte128(t);
}
}  // namespace

gsdb::registers::value gsdb::registers::read(const register_info& info) const {
    if (is_undefined(info.id)) {
        gsdb::error::send("Register is undefined");
    }

    auto bytes = as_bytes(data_);

    if (info.format == register_format::uint) {
        switch (info.size) {
            case 1:
                return from_bytes<std::uint8_t>(bytes + info.offset);
            case 2:
                return from_bytes<std::uint16_t>(bytes + info.offset);
            case 4:
                return from_bytes<std::uint32_t>(bytes + info.offset);
            case 8:
                return from_bytes<std::uint64_t>(bytes + info.offset);
            default:
                gsdb::error::send("Unexpected register size!");
        }
    } else if (info.format == register_format::double_float) {
        return from_bytes<double>(bytes + info.offset);
    } else if (info.format == register_format::long_double) {
        return from_bytes<long double>(bytes + info.offset);
    } else if (info.format == register_format::vector and info.size == 8) {
        return from_bytes<byte64>(bytes + info.offset);
    } else {
        return from_bytes<byte128>(bytes + info.offset);
    }
}

/**
 * Write all GPRs, FPRs, and debug registers
 */
void gsdb::registers::flush() {
    proc_->write_fprs(data_.i387, tid_);
    proc_->write_gprs(data_.regs, tid_);
    auto info = register_info_by_id(register_id::dr0);
    // Loop 8 times, one for each debug register
    // %dr0 - %dr7, for hardware breakpoints/watchpoints
    for (auto i = 0; i < 8; ++i) {
        // DR4 and DR5 are obsolete aliases for DR6 and DR7
        if (i == 4 or i == 5) continue;
        auto reg_offset = info.offset + sizeof(std::uint64_t) * i;
        auto ptr = reinterpret_cast<std::byte*>(data_.u_debugreg + i);
        auto bytes = from_bytes<std::uint64_t>(ptr);
        proc_->write_user_area(reg_offset, bytes, tid_);
    }
}

void gsdb::registers::write(const register_info& info, value val, bool commit) {
    auto bytes = as_bytes(data_);

    // copy the value the user passed into `bytes + info.offset`
    // std::visits preserves `val`'s type
    std::visit(
        // generic lambda
        [&](auto& v) {
            if (sizeof(v) <= info.size) {
                auto wide = widen(info, v);
                auto val_bytes = as_bytes(wide);
                std::copy(val_bytes, val_bytes + info.size,
                          bytes + info.offset);
            } else {
                std::cerr << "gsdb::registers::write called with mismatched "
                             "register and value sizes!";
                std::terminate();
            }
        },
        val);

    // Write the new register to the new process only if commit is true
    if (commit) {
        if (info.type == register_type::fpr) {
            proc_->write_fprs(data_.i387, tid_);
        } else {
            // PTRACE_PEEKUSER and PTRACE_POKEUSER require the addresses to
            // align to 8 bytes The high 8-bit registers are offset by a byte
            // into the superregister, so they aren’t aligned
            auto aligned_offset =
                info.offset & ~0b111;  // make it divisible by 8
            proc_->write_user_area(
                aligned_offset,
                from_bytes<std::uint64_t>(bytes + aligned_offset), tid_);
        }
    }
}

void gsdb::registers::undefine(gsdb::register_id id) {
    // shift the register offset 1 bit to the right so that registers at a byte
    // offset like `ah` are considered the same as their containing register
    std::size_t canonical_offset = register_info_by_id(id).offset >> 1;
    undefined_.push_back(canonical_offset);
}
bool gsdb::registers::is_undefined(gsdb::register_id id) const {
    // shift the register offset 1 bit to the right so that registers at a byte
    // offset like `hl` are considered the same as their containing register
    std::size_t canonical_offset = register_info_by_id(id).offset >> 1;
    return std::find(std::begin(undefined_), std::end(undefined_),
                     canonical_offset) != std::end(undefined_);
}
