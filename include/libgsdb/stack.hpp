#ifndef GSDB_STACK_HPP
#define GSDB_STACK_HPP

#include <cstdint>
#include <libgsdb/dwarf.hpp>
#include <vector>

namespace gsdb {
class target;
class stack {
   public:
    stack(target* tgt) : target_(tgt) {}
    /**
     * Called whenever process halts
     */
    void reset_inline_height();
    std::vector<gsdb::die> inline_stack_at_pc() const;
    std::uint32_t inline_height() const { return inline_height_; }
    const target& get_target() const { return *target_; }

    /**
     * Handle the situation in which execution is at the beginning of an inlined
     * function. We pretend to step into the function by decrementing the
     * current inline height and returning
     */
    void simulate_inlined_step_in() { --inline_height_; }

   private:
    // target to which this stack belongs
    target* target_ = nullptr;
    std::uint32_t inline_height_ = 0;
};
}  // namespace gsdb

#endif
