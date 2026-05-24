#include <cstddef>
#include <cstdint>
#include <cstring>
#include <libgsdb/error.hpp>
#include <libgsdb/process.hpp>
#include <libgsdb/watchpoint.hpp>
#include <utility>

#include "libgsdb/types.hpp"

namespace {
auto get_next_id() {
    static gsdb::watchpoint::id_type id = 0;
    return ++id;
}
}  // namespace

gsdb::watchpoint::watchpoint(gsdb::process& proc, virt_addr address,
                             stoppoint_mode mode, std::size_t size)
    : process_{&proc},
      address_{address},
      is_enabled_{false},
      mode_{mode},
      size_{size} {
    if ((address.addr() & (size - 1)) != 0) {
        error::send("Watchpoint must be aligned to size!");
    }
    id_ = get_next_id();
    update_data();
}

void gsdb::watchpoint::enable() {
    if (is_enabled_) {
        return;
    }

    hardware_register_index_ =
        process_->set_watchpoint(id_, address_, mode_, size_);
    is_enabled_ = true;
}

void gsdb::watchpoint::disable() {
    if (!is_enabled_) {
        return;
    }

    process_->clear_hardware_stoppoint(hardware_register_index_);
    is_enabled_ = false;
}

void gsdb::watchpoint::update_data() {
    std::uint64_t new_data = 0;
    auto read = process_->read_memory(address_, size_);
    memcpy(&new_data, read.data(), size_);
    previous_data_ = std::exchange(data_, new_data);
}
