#ifndef GSDB_STACK_HPP
#define GSDB_STACK_HPP

#include <cstddef>
#include <cstdint>
#include <libgsdb/dwarf.hpp>
#include <vector>

#include "libgsdb/registers.hpp"
#include "libgsdb/types.hpp"

namespace gsdb {
class target;

struct stack_frame {
    registers regs;
    virt_addr backtrace_report_address;
    die func_die;
    bool inlined = false;
    source_location location;
};

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
    void simulate_inlined_step_in() {
        --inline_height_;
        current_frame_ = inline_height_;
    }

    void unwind();
    void up() { ++current_frame_; }
    void down() { --current_frame_; }

    /**
     * Return the current set of frames, not including those that the compiler
     * inlined
     */
    span<const stack_frame> frames() const;
    bool has_frames() const { return !frames_.empty(); }
    const stack_frame& current_frame() const { return frames_[current_frame_]; }
    std::size_t current_frame_index() const {
        return current_frame_ - inline_height_;
    }
    const registers& regs() const;
    virt_addr get_pc() const;

   private:
    void create_inline_stack_frames(const gsdb::registers& regs,
                                    const std::vector<gsdb::die> inline_stack,
                                    file_addr pc);
    void create_base_frame(const registers& regs,
                           const std::vector<gsdb::die> inline_stack,
                           file_addr pc, bool inlined);
    // target to which this stack belongs
    target* target_ = nullptr;
    std::uint32_t inline_height_ = 0;
    // current set of stack frames
    std::vector<stack_frame> frames_;
    // the frame that the debugger is currently examining
    std::size_t current_frame_ = 0;
};
}  // namespace gsdb

#endif
