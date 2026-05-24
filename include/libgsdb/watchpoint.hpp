#ifndef GSDB_WATCHPOINT_HPP
#define GSDB_WATCHPOINT_HPP

#include <cstddef>
#include <cstdint>
#include <libgsdb/types.hpp>

namespace gsdb {
class process;
class watchpoint {
   public:
    // watchpoint is unique
    // can only be created through an `gsdb::process` object
    watchpoint() = delete;
    watchpoint(const watchpoint&) = delete;
    watchpoint& operator=(const watchpoint&) = delete;

    using id_type = std::int32_t;
    id_type id() const { return id_; }

    void enable();
    void disable();

    bool is_enabled() const { return is_enabled_; }
    virt_addr address() const { return address_; }
    stoppoint_mode mode() const { return mode_; }
    std::size_t size() const { return size_; }

    bool at_address(virt_addr addr) const { return address_ == addr; }
    bool in_range(virt_addr low, virt_addr high) const {
        return address_ >= low and high > address_;
    }

    std::uint64_t data() const { return data_; }
    std::uint64_t previous_data() const { return previous_data_; }

    // Re-read the value at the watched memory location
    void update_data();

   private:
    friend process;

    watchpoint(process& proc, virt_addr address, stoppoint_mode mode,
               std::size_t size);

    id_type id_;
    process* process_;
    virt_addr address_;
    bool is_enabled_;
    stoppoint_mode mode_;
    std::size_t size_;
    int hardware_register_index_ = -1;

    // track values to which the watchpoint point
    std::uint64_t data_ = 0;           // curr value at the watched address
    std::uint64_t previous_data_ = 0;  // previously read value
};

}  // namespace gsdb

#endif
