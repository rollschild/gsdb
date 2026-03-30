#ifndef GSDB_REGISTERS_HPP
#define GSDB_REGISTERS_HPP

#include <sys/user.h>

#include <cstdint>
#include <libgsdb/register_info.hpp>
#include <libgsdb/types.hpp>
#include <variant>

namespace gsdb {
class process;

class registers {
   public:
    registers() = delete;
    registers(const registers&) = delete;
    registers& operator=(const registers&) = delete;

    using value =
        std::variant<std::uint8_t, std::uint16_t, std::uint32_t, std::uint64_t,
                     std::int8_t, std::int16_t, std::int32_t, std::int64_t,
                     float, double, long double, byte64, byte128>;
    value read(const register_info& info) const;
    void write(const register_info& info, value val);

    // my_registers.read_by_id_as<std::uint64_t>(register_id::rax)
    template <class T>
    T read_by_id_as(register_id id) const {
        return std::get<T>(read(register_info_by_id(id)));
    }
    void write_by_id(register_id id, value val) {
        write(register_info_by_id(id), val);
    }

   private:
    // only a gsdb::process should be able to construct an `gsdb::registers`
    // object, thus declared as `friend`
    friend process;
    registers(process& proc) : proc_(&proc) {}

    user data_;
    process* proc_;
};
}  // namespace gsdb

#endif
