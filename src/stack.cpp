#include <cstdint>
#include <iterator>
#include <libgsdb/stack.hpp>
#include <libgsdb/target.hpp>
#include <vector>

#include "libgsdb/dwarf.hpp"
#include "libgsdb/register_info.hpp"
#include "libgsdb/registers.hpp"
#include "libgsdb/types.hpp"

std::vector<gsdb::die> gsdb::stack::inline_stack_at_pc() const {
    // program counter as file address
    auto pc = target_->get_pc_file_address(tid_);
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

gsdb::span<const gsdb::stack_frame> gsdb::stack::frames() const {
    // all frames below `inline_height_` in the frames_ member are inlined
    // frames that we are pretending the process has not yet entered
    return {frames_.data() + inline_height_, frames_.size() - inline_height_};
}

const gsdb::registers& gsdb::stack::regs() const {
    return frames_[current_frame_].regs;
}

gsdb::virt_addr gsdb::stack::get_pc() const {
    return virt_addr{regs().read_by_id_as<std::uint64_t>(register_id::rip)};
}

void gsdb::stack::unwind() {
    reset_inline_height();
    // so the selected frame is the first visible one
    current_frame_ = inline_height_;

    auto virt_pc = target_->get_process().get_pc(tid_);
    auto file_pc = target_->get_pc_file_address(tid_);
    auto& proc = target_->get_process();
    auto regs = proc.get_registers(tid_);

    frames_.clear();

    // Ensure that the program counter points to a valid ELF file
    auto elf = file_pc.elf_file();
    if (!elf) {
        return;
    }

    // Keep unwinding frames until we hit a frame with a program counter that
    // lives outside of the ELF file we’re operating on (indicating that this
    // function belongs to some shared library or that we’ve hit the topmost
    // frame):
    while (virt_pc.addr() != 0 and elf) {
        // Create stack_frame objects and unwind another frame
        // Grab DWARF info needed to calculate the inline stack
        auto& dwarf = elf->get_dwarf();
        // inline_stack vector is outermost -> innermost
        auto inline_stack = dwarf.inline_stack_at_address(file_pc);
        if (inline_stack.empty()) {
            return;
        }

        if (inline_stack.size() > 1) {
            // creates the frame for the bottommost function in the inline stack
            create_base_frame(regs, inline_stack, file_pc, true);
            // handles the additional multiple frames we need to create for
            // inline stacks
            create_inline_stack_frames(regs, inline_stack, file_pc);
        } else {
            create_base_frame(regs, inline_stack, file_pc, false);
        }

        // after updating `frames_`, unwind another frame using the call frame
        // information and update the program counter values that the loop uses
        regs = dwarf.cfi().unwind(proc, file_pc, frames_.back().regs);
        virt_pc =
            virt_addr{regs.read_by_id_as<std::uint64_t>(register_id::rip) - 1};
        file_pc = virt_pc.to_file_addr(target_->get_elves());
        elf = file_pc.elf_file();
    }
}

/*
 * Emits the innermost logical frame
 */
void gsdb::stack::create_base_frame(const registers& regs,
                                    const std::vector<gsdb::die> inline_stack,
                                    file_addr pc, bool inlined) {
    auto backtrace_pc = pc.to_virt_addr();
    // Find the start of the call instruction (start of the row for this source
    // line), by finding the line table entry that corresponds to that
    // instruction, then calculating its start address as a virtual address
    auto line_entry = pc.elf_file()->get_dwarf().line_entry_at_address(pc);
    if (line_entry != line_table::iterator{}) {
        backtrace_pc = line_entry->address.to_virt_addr();
    }

    // FIXME
    frames_.push_back(
        {regs, backtrace_pc, inline_stack.back(), inlined,
         source_location{line_entry->file_entry, line_entry->line}});
}

/**
 * Emits the remaining logical frames for the _same_ physical frame, walking
 * outward from the second innermost entry
 */
void gsdb::stack::create_inline_stack_frames(
    const registers& regs, const std::vector<gsdb::die> inline_stack,
    [[maybe_unused]] file_addr pc) {
    // walk backward over the frames of the inline stack
    for (auto it = inline_stack.rbegin() + 1; it != inline_stack.rend(); ++it) {
        auto inlined_pc = std::prev(it)->low_pc().to_virt_addr();
        frames_.push_back(stack_frame{regs, inlined_pc, *it,
                                      std::next(it) != inline_stack.rend(),
                                      std::prev(it)->location()});
    }
}
