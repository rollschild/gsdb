#ifndef GSDB_REGISTERS_HPP
#define GSDB_REGISTERS_HPP

#include <sys/user.h>

#include <cstddef>
#include <cstdint>
#include <libgsdb/register_info.hpp>
#include <libgsdb/types.hpp>
#include <variant>
#include <vector>

namespace gsdb {
class process;

class registers {
   public:
    registers() = default;
    registers(const registers&) = default;
    registers& operator=(const registers&) = default;

    using value =
        std::variant<std::uint8_t, std::uint16_t, std::uint32_t, std::uint64_t,
                     std::int8_t, std::int16_t, std::int32_t, std::int64_t,
                     float, double, long double, byte64, byte128>;
    value read(const register_info& info) const;
    // commit: whether to also write to the registers in the inferior
    void write(const register_info& info, value val, bool commit = true);

    // my_registers.read_by_id_as<std::uint64_t>(register_id::rax)
    template <class T>
    T read_by_id_as(register_id id) const {
        return std::get<T>(read(register_info_by_id(id)));
    }
    void write_by_id(register_id id, value val, bool commit = true) {
        write(register_info_by_id(id), val, commit);
    }
    bool is_undefined(register_id id) const;
    void undefine(register_id id);

    virt_addr cfa() const { return cfa_; }
    void set_cfa(virt_addr addr) { cfa_ = addr; }
    // write all current registers back to the process
    void flush();

   private:
    // only a gsdb::process should be able to construct an `gsdb::registers`
    // object, thus declared as `friend`
    friend process;
    registers(process& proc) : proc_(&proc) {}

    user data_;
    process* proc_;

    /**
     * List of offsets used as a stand-in for "which physical register is
     * undefined" (DWARF CFI can mark a register as having no recoverable value
     * in a frame)
     */
    std::vector<std::size_t> undefined_;
    virt_addr cfa_;
};
}  // namespace gsdb

#endif
