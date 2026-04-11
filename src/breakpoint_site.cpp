#include <sys/ptrace.h>

#include <cerrno>
#include <cstddef>
#include <libgsdb/breakpoint_site.hpp>

#include "libgsdb/error.hpp"
#include "libgsdb/process.hpp"
#include "libgsdb/types.hpp"

namespace {
auto get_next_id() {
    // initialized exactly once
    static gsdb::breakpoint_site::id_type id = 0;
    return ++id;
}
}  // namespace

gsdb::breakpoint_site::breakpoint_site(process& proc, virt_addr address)
    : process_{&proc}, address_{address}, is_enabled_{false}, saved_data_{} {
    id_ = get_next_id();
}

void gsdb::breakpoint_site::enable() {
    if (is_enabled_) return;

    errno = 0;

    // read 64 bits of data from address at which we need to set the breakpoint
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

    is_enabled_ = true;
}

void gsdb::breakpoint_site::disable() {
    if (!is_enabled_) return;

    errno = 0;
    std::uint64_t data =
        ptrace(PTRACE_PEEKDATA, process_->pid(), address_, nullptr);
    if (errno != 0) {
        error::send_errno("Disabling breakpoint site failed!");
    }

    auto restored_data =
        ((data & ~0xff) | static_cast<std::uint8_t>(saved_data_));
    // write back restored data
    if (ptrace(PTRACE_POKEDATA, process_->pid(), address_, restored_data) < 0) {
        error::send_errno("Disabling breakpoint site failed!");
    }

    is_enabled_ = false;
}
