#include <elf.h>

#include <algorithm>
#include <csignal>
#include <filesystem>
#include <libgsdb/target.hpp>
#include <libgsdb/types.hpp>
#include <memory>
#include <string>
#include <utility>

#include "libgsdb/breakpoint_site.hpp"
#include "libgsdb/disassembler.hpp"
#include "libgsdb/dwarf.hpp"
#include "libgsdb/elf.hpp"
#include "libgsdb/process.hpp"

namespace {
/**
 * Used inside both `launch` and `attach`.
 * Get the auxiliary vector for the process.
 * Create an `gsdb::elf` object for the file at the given path.
 * Then set the load address of its `.text` section
 */
std::unique_ptr<gsdb::elf> create_loaded_elf(
    const gsdb::process& proc, const std::filesystem::path& path) {
    auto auxv = proc.get_auxv();
    auto obj = std::make_unique<gsdb::elf>(path);
    obj->notify_loaded(
        // runtime virtual address = ELF file address + load_bias
        // ELF file address        = runtime virtual address - load_bias
        // By subtracting the load address of the entry point in the ELF header
        // from the actual load address of the entry point
        gsdb::virt_addr(auxv[AT_ENTRY] - obj->get_header().e_entry));
    return obj;
}
}  // namespace

std::unique_ptr<gsdb::target> gsdb::target::launch(
    std::filesystem::path path, std::optional<int> stdout_replacement) {
    auto proc = process::launch(path, true, stdout_replacement);
    auto obj = create_loaded_elf(*proc, path);
    auto tgt =
        std::unique_ptr<target>(new target(std::move(proc), std::move(obj)));
    tgt->get_process().set_target(tgt.get());
    return tgt;
}

std::unique_ptr<gsdb::target> gsdb::target::attach(pid_t pid) {
    // Joining paths.
    // The `/proc/<pid>/exe` file is a symbolic link to the executable for the
    // given process.
    auto elf_path =
        std::filesystem::path("/proc/") / std::to_string(pid) / "exe";
    auto proc = process::attach(pid);
    auto obj = create_loaded_elf(*proc, elf_path);
    auto tgt =
        std::unique_ptr<target>(new target(std::move(proc), std::move(obj)));
    tgt->get_process().set_target(tgt.get());
    return tgt;
}

gsdb::file_addr gsdb::target::get_pc_file_address() const {
    return process_->get_pc().to_file_addr(*elf_);
}

/**
 * Everyt time the process halts, recalculate the inline height
 */
void gsdb::target::notify_stop(
    [[maybe_unused]] const gsdb::stop_reason& reason) {
    stack_.reset_inline_height();
}

gsdb::stop_reason gsdb::target::step_in() {
    auto& stack = get_stack();
    if (stack.inline_height() > 0) {
        stack.simulate_inlined_step_in();
        return stop_reason(process_state::stopped, SIGTRAP,
                           trap_type::single_step);
    }

    // Line entry to which the program counter is currently pointing
    auto orig_line = line_entry_at_pc();
    do {
        // step over a single instruction
        // store the reason why execution stopped
        auto reason = process_->step_instruction();
        // program may have stopped at a breakpoint or terminated completely
        if (!reason.is_step()) {
            return reason;
        }
    } /* The loop terminates when the line entry corresponding to the current
         program counter differs from the one we stored at the start of the
         operation. */
    while ((line_entry_at_pc() == orig_line or
            // if line entry is special end-of-sequence marker, we keep stepping
            // as the marker doesn't correspond to an actual line of source code
            line_entry_at_pc()->end_sequence) and
           line_entry_at_pc() != line_table::iterator{});

    // Now execution will have reached a new line of source code.
    // But still need to step over the function prologue if we’ve entered a new
    // function.
    auto pc = get_pc_file_address();
    if (pc.elf_file() != nullptr) {
        auto& dwarf = pc.elf_file()->get_dwarf();
        // find the function containing the program counter offset
        auto func = dwarf.function_containing_address(pc);
        // If the program counter is at the start of that function's range, we
        // know we've encountered the prologue of a function
        if (func and func->low_pc() == pc) {
            auto line = line_entry_at_pc();
            if (line != line_table::iterator{}) {
                ++line;  // marking the start of the function body
                return run_until_address(line->address.to_virt_addr());
            }
        }
    }

    return stop_reason(process_state::stopped, SIGTRAP, trap_type::single_step);
}

gsdb::line_table::iterator gsdb::target::line_entry_at_pc() const {
    auto pc = get_pc_file_address();
    // pc might be empty file address - e.g. if function currently being
    // executed belongs to a shared lib
    if (!pc.elf_file()) return line_table::iterator();
    auto cu = pc.elf_file()->get_dwarf().compile_unit_containing_address(pc);
    if (!cu) return line_table::iterator();
    // return entry corresponding to the current program counter in the correct
    // compile unit
    return cu->lines().get_entry_by_address(pc);
}

gsdb::stop_reason gsdb::target::run_until_address(virt_addr address) {
    breakpoint_site* breakpoint_to_remove = nullptr;
    if (!process_->breakpoint_sites().contains_address(address)) {
        breakpoint_to_remove =
            &process_->create_breakpoint_site(address, false, true);
        breakpoint_to_remove->enable();
    }

    process_->resume();
    auto reason = process_->wait_on_signal();
    // process may halt for other reasons - check the reason first
    if (reason.is_breakpoint() and process_->get_pc() == address) {
        reason.trap_reason = trap_type::single_step;
    }

    if (breakpoint_to_remove) {
        process_->breakpoint_sites().remove_by_address(
            breakpoint_to_remove->address());
    }

    return reason;
}

gsdb::stop_reason gsdb::target::step_over() {
    auto orig_line = line_entry_at_pc();
    // to determine whether the next instruction to be executed is a function
    // call
    disassembler disas(*process_);
    gsdb::stop_reason reason;
    auto& stack = get_stack();
    do {
        auto inline_stack = stack.inline_stack_at_pc();
        // whether the stack contains any inline frames
        auto at_start_of_inline_frame = stack.inline_height() > 0;
        if (at_start_of_inline_frame) {
            // if inline frames, whether we are at the start of _one of_ them
            auto frame_to_skip =
                inline_stack[inline_stack.size() - stack.inline_height()];
            auto return_address = frame_to_skip.high_pc().to_virt_addr();
            reason = run_until_address(return_address);
            if (!reason.is_step() or process_->get_pc() != return_address) {
                return reason;
            }
        } else if (auto instructions = disas.disassemble(2, process_->get_pc());
                   /* instructions[0].text.rfind("call") == 0*/ instructions[0]
                       .text.starts_with("call")) {
            reason = run_until_address(instructions[1].address);
            if (!reason.is_step() or
                process_->get_pc() != instructions[1].address) {
                return reason;
            }
        } else {
            reason = process_->step_instruction();
            if (!reason.is_step()) {
                return reason;
            }
        }
    } while ((line_entry_at_pc() == orig_line or
              line_entry_at_pc()->end_sequence) and
             line_entry_at_pc() !=
                 line_table::iterator{});  // until execution reaches a new line
                                           // table entry that is _not_ an end
                                           // sequence marker

    return reason;
}
