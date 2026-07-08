#include <sys/ptrace.h>

#include <cerrno>
#include <cstddef>
#include <libgsdb/breakpoint_site.hpp>

#include "libgsdb/error.hpp"
#include "libgsdb/process.hpp"
#include "libgsdb/types.hpp"

namespace {
[[maybe_unused]]
auto get_next_id() {
    // initialized exactly once
    static gsdb::breakpoint_site::id_type id = 0;
    return ++id;
}
}  // namespace

gsdb::breakpoint_site::breakpoint_site(process& proc, virt_addr address,
                                       bool is_hardware, bool is_internal)
    : process_{&proc},
      address_{address},
      is_enabled_{false},
      saved_data_{},
      is_hardware_{is_hardware},
      is_internal_{is_internal} {
    id_ = is_internal_ ? -1 : get_next_id();
}
gsdb::breakpoint_site::breakpoint_site(gsdb::breakpoint* parent, id_type id,
                                       process& proc, virt_addr address,
                                       bool is_hardware, bool is_internal)
    : parent_{parent},
      id_(id),
      process_{&proc},
      address_{address},
      is_enabled_{false},
      saved_data_{},
      is_hardware_(is_hardware),
      is_internal_(is_internal) {
    // id_ = is_internal_ ? -1 : get_next_id();
}

/* On x86-64, the CPU's execution cycle is: fetch the instruction
   at rip, advance rip past it, then execute. So when the CPU
  hits the 0xcc byte at, say, address 0x4000:

  1. It fetches the 1-byte int3 at rip = 0x4000
  2. It advances rip to 0x4001 (past the 1-byte instruction)
  3. It executes int3, which raises SIGTRAP and stops the
  process

  By the time the debugger gets control, rip is already 0x4001 —
   one byte past where the breakpoint was placed. The original
  instruction that lived at 0x4000 hasn't executed at all (it
  was replaced by 0xcc), but the PC has moved past it.

  That's why the fix-up subtracts 1: to point rip back at 0x4000
   so the debugger can restore the original byte and re-execute
  the real instruction. */
void gsdb::breakpoint_site::enable() {
    if (is_enabled_) return;
    if (is_hardware_) {
        hardware_register_index_ =
            process_->set_hardware_breakpoint(id_, address_);
    } else {
        errno = 0;

        // read 64 bits of data from address at which we need to set the
        // breakpoint
        std::uint64_t data =
            ptrace(PTRACE_PEEKDATA, process_->pid(), address_, nullptr);
        if (errno != 0) {
            error::send_errno("Enabling breakpoint site failed!");
        }

        // we need only the first 8 bits - save those bits
        saved_data_ = static_cast<std::byte>(data & 0xff);

        // replace first 8 bits with 0xcc
        std::uint64_t int3 = 0xcc;
        std::uint64_t data_with_int3 = ((data & ~0xff) | int3);

        // write to memory
        if (ptrace(PTRACE_POKEDATA, process_->pid(), address_, data_with_int3) <
            0) {
            error::send_errno("Enabling breakpoint site failed!");
        }
    }

    is_enabled_ = true;
}

void gsdb::breakpoint_site::disable() {
    if (!is_enabled_) return;
    if (is_hardware_) {
        process_->clear_hardware_stoppoint(hardware_register_index_);
        hardware_register_index_ = -1;
    } else {
        errno = 0;
        std::uint64_t data =
            ptrace(PTRACE_PEEKDATA, process_->pid(), address_, nullptr);
        if (errno != 0) {
            error::send_errno("Disabling breakpoint site failed!");
        }

        auto restored_data =
            ((data & ~0xff) | static_cast<std::uint8_t>(saved_data_));
        // write back restored data
        if (ptrace(PTRACE_POKEDATA, process_->pid(), address_, restored_data) <
            0) {
            error::send_errno("Disabling breakpoint site failed!");
        }
    }

    is_enabled_ = false;
}
