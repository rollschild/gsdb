#include <cxxabi.h>
#include <elf.h>

#include <algorithm>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <libgsdb/target.hpp>
#include <libgsdb/types.hpp>
#include <memory>
#include <string>
#include <utility>

#include "libgsdb/breakpoint.hpp"
#include "libgsdb/breakpoint_site.hpp"
#include "libgsdb/disassembler.hpp"
#include "libgsdb/dwarf.hpp"
#include "libgsdb/elf.hpp"
#include "libgsdb/process.hpp"
#include "libgsdb/register_info.hpp"

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
 * Everyt time the process halts, recalculate the inline height.
 * Call this function every time the process stops and that it should recompute
 * the current set of stack frames.
 */
void gsdb::target::notify_stop(
    [[maybe_unused]] const gsdb::stop_reason& reason) {
    // stack_.reset_inline_height();
    stack_.unwind();
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

gsdb::stop_reason gsdb::target::step_out() {
    auto& stack = get_stack();
    auto inline_stack = stack.inline_stack_at_pc();
    auto has_inline_frames = inline_stack.size() > 1;
    auto at_inline_frame = stack.inline_height() < inline_stack.size() - 1;

    if (has_inline_frames and at_inline_frame) {
        auto current_frame =
            inline_stack[inline_stack.size() - stack.inline_height() - 1];
        auto return_address = current_frame.high_pc().to_virt_addr();
        return run_until_address(return_address);
    }

    // grab the stack frame above this one, and retrieve the PC value for that
    // frame, which will be the return address for the current frame
    auto& regs = stack.frames()[stack.current_frame_index() + 1].regs;
    virt_addr return_address{
        regs.read_by_id_as<std::uint64_t>(register_id::rip)};
    stop_reason reason;
    // Keep running up to that address until the number of stack frames is less
    // than it was at the start of the stepping procedure
    for (auto frames = stack.frames().size();
         stack.frames().size() >= frames;) {
        reason = run_until_address(return_address);
        if (!reason.is_breakpoint() or process_->get_pc() != return_address) {
            return reason;
        }
    }
    return reason;
}

gsdb::target::find_functions_result gsdb::target::find_functions(
    std::string name) const {
    find_functions_result res;

    // locate functions
    auto dwarf_found = elf_->get_dwarf().find_functions(name);
    if (dwarf_found.empty()) {
        // if no functions found, look them up in the ELF symbol table
        auto elf_found = elf_->get_symbols_by_name(name);
        for (auto sym : elf_found) {
            res.elf_functions.push_back(std::pair{elf_.get(), sym});
        }
    } else {
        res.dwarf_functions.insert(res.dwarf_functions.end(),
                                   dwarf_found.begin(), dwarf_found.end());
    }

    return res;
}

gsdb::breakpoint& gsdb::target::create_address_breakpoint(
    gsdb::virt_addr address, bool hardware, bool internal) {
    return breakpoints_.push(std::unique_ptr<address_breakpoint>(
        new address_breakpoint(*this, address, hardware, internal)));
}
gsdb::breakpoint& gsdb::target::create_function_breakpoint(
    std::string function_name, bool hardware, bool internal) {
    return breakpoints_.push(std::unique_ptr<function_breakpoint>(
        new function_breakpoint(*this, function_name, hardware, internal)));
}
gsdb::breakpoint& gsdb::target::create_line_breakpoint(
    std::filesystem::path file, std::size_t line, bool hardware,
    bool internal) {
    return breakpoints_.push(std::unique_ptr<line_breakpoint>(
        new line_breakpoint(*this, file, line, hardware, internal)));
}

/**
 * Find the function DIE containing the program counter as a file address
 */
std::string gsdb::target::function_name_at_address(
    gsdb::virt_addr address) const {
    auto file_address = address.to_file_addr(*elf_);
    auto obj = file_address.elf_file();
    if (!obj) return "";

    auto func = obj->get_dwarf().function_containing_address(file_address);
    if (func and func->name()) {
        return std::string(*func->name());
    } else if (auto elf_func = obj->get_symbol_containing_address(file_address);
               elf_func and
               ELF64_ST_TYPE(elf_func.value()->st_info) == STT_FUNC) {
        // Look for the ELF symbol containing the current program counter as an
        // offset, and if we find one that is a function symbol, demangle it and
        // return it
        auto elf_name = std::string{obj->get_string(elf_func.value()->st_name)};
        return abi::__cxa_demangle(elf_name.c_str(), nullptr, nullptr, nullptr);
    }

    return "";
}
