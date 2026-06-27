#include <libgsdb/stack.hpp>
#include <libgsdb/target.hpp>
#include <vector>

#include "libgsdb/dwarf.hpp"

std::vector<gsdb::die> gsdb::stack::inline_stack_at_pc() const {
    // program counter as file address
    auto pc = target_->get_pc_file_address();
    if (!pc.elf_file()) {
        return {};
    }

    return pc.elf_file()->get_dwarf().inline_stack_at_address(pc);
}

/**
 * Walk backward every inline stack frame for which execution is at the
 * beginning, incrementing the current inline height each time
 */
void gsdb::stack::reset_inline_height() {
    auto stack = inline_stack_at_pc();

    inline_height_ = 0;  // pointing to the deepest inline function
    auto pc = target_->get_pc_file_address();

    // reverse iterators - iterate backward
    // starting at the deepest one, until we hit either the beginning or a frame
    // of which execution isn't at the start
    for (auto it = stack.rbegin(); it != stack.rend() and it->low_pc() == pc;
         ++it) {
        // if the PC sits **exactly on that inlined function's first
        // instruction**.
        ++inline_height_;
    }
}
