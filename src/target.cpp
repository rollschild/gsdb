#include <cxxabi.h>
#include <elf.h>

#include <algorithm>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <libgsdb/target.hpp>
#include <libgsdb/types.hpp>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "libgsdb/breakpoint.hpp"
#include "libgsdb/breakpoint_site.hpp"
#include "libgsdb/disassembler.hpp"
#include "libgsdb/dwarf.hpp"
#include "libgsdb/elf.hpp"
#include "libgsdb/process.hpp"
#include "libgsdb/register_info.hpp"
#include "libgsdb/stack.hpp"

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

std::filesystem::path dump_vdso(const gsdb::process& proc,
                                gsdb::virt_addr address) {
    char tmp_dir[] =
        "/tmp/gsdb-XXXXXX";  // XXXXXX fill be filled with random characters
    mkdtemp(tmp_dir);
    auto vdso_dump_path = std::filesystem::path(tmp_dir) / "linux-vdso.so.1";
    std::ofstream vdso_dump(vdso_dump_path, std::ios::binary);
    auto vdso_header = proc.read_memory_as<Elf64_Ehdr>(address);
    // vDSO does have section headers
    // offset the start of the section headers by the number of section entries
    // multiplied by the size of an entry
    auto vdso_size =
        vdso_header.e_shoff + vdso_header.e_shentsize * vdso_header.e_shnum;
    auto vdso_bytes = proc.read_memory(address, vdso_size);
    vdso_dump.write(reinterpret_cast<const char*>(vdso_bytes.data()),
                    vdso_bytes.size());
    return vdso_dump_path;
}
}  // namespace

std::unique_ptr<gsdb::target> gsdb::target::launch(
    std::filesystem::path path, std::optional<int> stdout_replacement) {
    auto proc = process::launch(path, true, stdout_replacement);
    auto obj = create_loaded_elf(*proc, path);
    auto tgt =
        std::unique_ptr<target>(new target(std::move(proc), std::move(obj)));
    tgt->get_process().set_target(tgt.get());

    // retrieve the real entry point of the executable from the auxiliary vector
    auto entry_point = virt_addr{tgt->get_process().get_auxv()[AT_ENTRY]};
    auto& entry_bp = tgt->create_address_breakpoint(entry_point, false, true);
    entry_bp.install_hit_handler([target = tgt.get()] {
        target->resolve_dynamic_linker_rendezvous();
        // the process should restart immediately after resolving the address of
        // the rendezvous structure
        return true;
    });
    entry_bp.enable();
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
    tgt->resolve_dynamic_linker_rendezvous();
    return tgt;
}

gsdb::file_addr gsdb::target::get_pc_file_address(
    std::optional<pid_t> otid) const {
    return process_->get_pc(otid).to_file_addr(elves_);
}

/**
 * Everyt time the process halts, recalculate the inline height.
 * Call this function every time the process stops and that it should recompute
 * the current set of stack frames.
 */
void gsdb::target::notify_stop(const gsdb::stop_reason& reason) {
    threads_.at(reason.tid).frames.unwind();
}

gsdb::stop_reason gsdb::target::step_in(std::optional<pid_t> otid) {
    auto tid = otid.value_or(process_->current_thread());
    auto& stack = get_stack(tid);
    auto& thread = threads_.at(tid);
    if (stack.inline_height() > 0) {
        stack.simulate_inlined_step_in();
        stop_reason reason(tid, process_state::stopped, SIGTRAP,
                           trap_type::single_step);
        thread.state->reason = reason;
        return reason;
    }

    // Line entry to which the program counter is currently pointing
    auto orig_line = line_entry_at_pc(tid);
    do {
        // step over a single instruction
        // store the reason why execution stopped
        auto reason = process_->step_instruction(tid);
        // program may have stopped at a breakpoint or terminated completely
        if (!reason.is_step()) {
            thread.state->reason = reason;
            return reason;
        }
    } /* The loop terminates when the line entry corresponding to the current
         program counter differs from the one we stored at the start of the
         operation. */
    while ((line_entry_at_pc(tid) == orig_line or
            // if line entry is special end-of-sequence marker, we keep stepping
            // as the marker doesn't correspond to an actual line of source code
            line_entry_at_pc(tid)->end_sequence) and
           line_entry_at_pc(tid) != line_table::iterator{});

    // Now execution will have reached a new line of source code.
    // But still need to step over the function prologue if we’ve entered a new
    // function.
    auto pc = get_pc_file_address(tid);
    if (pc.elf_file() != nullptr) {
        auto& dwarf = pc.elf_file()->get_dwarf();
        // find the function containing the program counter offset
        auto func = dwarf.function_containing_address(pc);
        // If the program counter is at the start of that function's range, we
        // know we've encountered the prologue of a function
        if (func and func->low_pc() == pc) {
            auto line = line_entry_at_pc(tid);
            if (line != line_table::iterator{}) {
                ++line;  // marking the start of the function body
                return run_until_address(line->address.to_virt_addr(), tid);
            }
        }
    }

    stop_reason reason(tid, process_state::stopped, SIGTRAP,
                       trap_type::single_step);
    thread.state->reason = reason;
    return reason;
}

gsdb::line_table::iterator gsdb::target::line_entry_at_pc(
    std::optional<pid_t> otid) const {
    auto pc = get_pc_file_address(otid);
    // pc might be empty file address - e.g. if function currently being
    // executed belongs to a shared lib
    if (!pc.elf_file()) return line_table::iterator();
    auto cu = pc.elf_file()->get_dwarf().compile_unit_containing_address(pc);
    if (!cu) return line_table::iterator();
    // return entry corresponding to the current program counter in the correct
    // compile unit
    return cu->lines().get_entry_by_address(pc);
}

gsdb::stop_reason gsdb::target::run_until_address(virt_addr address,
                                                  std::optional<pid_t> otid) {
    auto tid = otid.value_or(process_->current_thread());
    breakpoint_site* breakpoint_to_remove = nullptr;
    if (!process_->breakpoint_sites().contains_address(address)) {
        breakpoint_to_remove =
            &process_->create_breakpoint_site(address, false, true);
        breakpoint_to_remove->enable();
    }

    process_->resume(tid);
    auto reason = process_->wait_on_signal(tid);
    // process may halt for other reasons - check the reason first
    if (reason.is_breakpoint() and process_->get_pc(tid) == address) {
        reason.trap_reason = trap_type::single_step;
    }

    if (breakpoint_to_remove) {
        process_->breakpoint_sites().remove_by_address(
            breakpoint_to_remove->address());
    }

    threads_.at(tid).state->reason = reason;
    return reason;
}

gsdb::stop_reason gsdb::target::step_over(std::optional<pid_t> otid) {
    auto tid = otid.value_or(process_->current_thread());
    auto& thread = threads_.at(tid);
    auto& stack = get_stack(tid);
    auto orig_line = line_entry_at_pc(tid);
    // to determine whether the next instruction to be executed is a function
    // call
    disassembler disas(*process_);
    gsdb::stop_reason reason;
    do {
        auto inline_stack = stack.inline_stack_at_pc();
        // whether the stack contains any inline frames
        auto at_start_of_inline_frame = stack.inline_height() > 0;
        if (at_start_of_inline_frame) {
            // if inline frames, whether we are at the start of _one of_ them
            auto frame_to_skip =
                inline_stack[inline_stack.size() - stack.inline_height()];
            auto return_address = frame_to_skip.high_pc().to_virt_addr();
            reason = run_until_address(return_address, tid);
            if (!reason.is_step() or process_->get_pc(tid) != return_address) {
                thread.state->reason = reason;
                return reason;
            }
        } else if (auto instructions =
                       disas.disassemble(2, process_->get_pc(tid));
                   /* instructions[0].text.rfind("call") == 0*/ instructions[0]
                       .text.starts_with("call")) {
            reason = run_until_address(instructions[1].address, tid);
            if (!reason.is_step() or
                process_->get_pc(tid) != instructions[1].address) {
                thread.state->reason = reason;
                return reason;
            }
        } else {
            reason = process_->step_instruction(tid);
            if (!reason.is_step()) {
                thread.state->reason = reason;
                return reason;
            }
        }
    } while ((line_entry_at_pc(tid) == orig_line or
              line_entry_at_pc(tid)->end_sequence) and
             line_entry_at_pc(tid) !=
                 line_table::iterator{});  // until execution reaches a new line
                                           // table entry that is _not_ an end
                                           // sequence marker

    thread.state->reason = reason;
    return reason;
}

gsdb::stop_reason gsdb::target::step_out(std::optional<pid_t> otid) {
    auto tid = otid.value_or(process_->current_thread());
    auto& stack = get_stack(tid);
    auto inline_stack = stack.inline_stack_at_pc();
    auto has_inline_frames = inline_stack.size() > 1;
    auto at_inline_frame = stack.inline_height() < inline_stack.size() - 1;

    if (has_inline_frames and at_inline_frame) {
        auto current_frame =
            inline_stack[inline_stack.size() - stack.inline_height() - 1];
        auto return_address = current_frame.high_pc().to_virt_addr();
        return run_until_address(return_address, tid);
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
        reason = run_until_address(return_address, tid);
        if (!reason.is_breakpoint() or process_->get_pc() != return_address) {
            return reason;
        }
    }
    return reason;
}

gsdb::target::find_functions_result gsdb::target::find_functions(
    std::string name) const {
    find_functions_result res;

    elves_.for_each([&](auto& elf) {
        auto dwarf_found = elf.get_dwarf().find_functions(name);
        if (dwarf_found.empty()) {
            auto elf_found = elf.get_symbols_by_name(name);
            for (auto sym : elf_found) {
                res.elf_functions.push_back(std::pair{&elf, sym});
            }
        } else {
            res.dwarf_functions.insert(res.dwarf_functions.end(),
                                       dwarf_found.begin(), dwarf_found.end());
        }
    });

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
    auto file_address = address.to_file_addr(elves_);
    auto obj = file_address.elf_file();
    if (!obj) return "";

    auto func = obj->get_dwarf().function_containing_address(file_address);
    auto elf_filename = obj->path().filename().string();
    std::string func_name = "";
    if (func and func->name()) {
        func_name = *func->name();
    } else if (auto elf_func = obj->get_symbol_containing_address(file_address);
               elf_func and
               ELF64_ST_TYPE(elf_func.value()->st_info) == STT_FUNC) {
        // Look for the ELF symbol containing the current program counter as an
        // offset, and if we find one that is a function symbol, demangle it and
        // return it
        auto elf_name = std::string{obj->get_string(elf_func.value()->st_name)};
        func_name = obj->get_string(elf_func.value()->st_name);
        // return abi::__cxa_demangle(elf_name.c_str(), nullptr, nullptr,
        // nullptr);
    }

    if (!func_name.empty()) {
        return elf_filename + "`" + func_name;
    }

    return "";
}

void gsdb::target::resolve_dynamic_linker_rendezvous() {
    // immediately return if address of the rendezvous structure already solved
    if (dynamic_linker_rendezvous_address_.addr()) return;

    auto dynamic_section = main_elf_->get_section(".dynamic");
    auto dynamic_start =
        file_addr{*main_elf_, dynamic_section.value()->sh_addr};
    auto dynamic_size = dynamic_section.value()->sh_size;
    auto dynamic_bytes =
        process_->read_memory(dynamic_start.to_virt_addr(), dynamic_size);

    // typedef struct {
    //     Elf64_Sxword d_tag;
    //
    //        union {
    //            Elf64_Xword d_val;
    //            Elf64_Addr d_ptr;
    //     } d_un;
    // } Elf64_Dyn;
    std::vector<Elf64_Dyn> dynamic_entries(dynamic_size / sizeof(Elf64_Dyn));
    std::copy(dynamic_bytes.begin(), dynamic_bytes.end(),
              reinterpret_cast<std::byte*>(dynamic_entries.data()));

    for (auto entry : dynamic_entries) {
        if (entry.d_tag == DT_DEBUG) {
            // `DT_DEBUG` entry stores the rendezvous structure's address
            dynamic_linker_rendezvous_address_ = virt_addr{entry.d_un.d_ptr};
            // initialize a list of the loaded libraries
            reload_dynamic_libraries();

            auto debug_info = read_dynamic_linker_rendezvous();
            // `r_brk` member of the rendezvous structure holds pointer to
            // function `_dl_debug_state`
            auto debug_state_addr = virt_addr{debug_info->r_brk};
            auto& debug_state_bp =
                create_address_breakpoint(debug_state_addr, false, true);
            debug_state_bp.install_hit_handler([&] {
                reload_dynamic_libraries();
                return true;
            });

            debug_state_bp.enable();
        }
    }
}

/**
 * Look for line entries in any ELF file
 */
std::vector<gsdb::line_table::iterator> gsdb::target::get_line_entries_by_line(
    std::filesystem::path path, std::size_t line) const {
    std::vector<gsdb::line_table::iterator> entries;
    elves_.for_each([&](auto& elf) {
        for (auto& cu : elf.get_dwarf().compile_units()) {
            auto new_entries = cu->lines().get_entries_by_line(path, line);
            entries.insert(entries.end(), new_entries.begin(),
                           new_entries.end());
        }
    });
    return entries;
}

std::optional<r_debug> gsdb::target::read_dynamic_linker_rendezvous() const {
    if (dynamic_linker_rendezvous_address_.addr()) {
        return process_->read_memory_as<r_debug>(
            dynamic_linker_rendezvous_address_);
    }
    return std::nullopt;
}

void gsdb::target::reload_dynamic_libraries() {
    auto debug = read_dynamic_linker_rendezvous();
    if (!debug) return;

    auto entry_ptr = debug->r_map;
    // loop until we hit the end of the list
    while (entry_ptr != nullptr) {
        auto entry_addr = virt_addr(reinterpret_cast<std::uint64_t>(entry_ptr));
        auto entry = process_->read_memory_as<link_map>(entry_addr);
        entry_ptr = entry.l_next;

        // read path to the ELF file, encoded as null-terminated string in the
        // `l_name` field of the map entry
        auto name_addr =
            virt_addr(reinterpret_cast<std::uint64_t>(entry.l_name));
        // max path size on linux is 4096 bytes
        auto name_bytes = process_->read_memory(name_addr, 4096);
        auto name =
            std::filesystem::path{reinterpret_cast<char*>(name_bytes.data())};
        if (name.empty()) continue;

        // check whether we've already created an `gsdb::elf` object for this
        // shared lib
        const elf* found = nullptr;
        const auto vdso_name = "linux-vdso.so.1";
        // should be an absolute path to the ELF file for that share lib, except
        // for vDSO
        if (name == vdso_name) {
            found = elves_.get_elf_by_filename(name.c_str());
        } else {
            found = elves_.get_elf_by_path(name);
        }

        // if didn't find a corresponding `gsdb::elf` object, create one
        if (!found) {
            if (name == vdso_name) {
                // if this entry is for vDSO, first dump the vDSO to disk
                name = dump_vdso(*process_, virt_addr{entry.l_addr});
            }
            auto new_elf = std::make_unique<elf>(name);
            new_elf->notify_loaded(virt_addr{entry.l_addr});
            elves_.push(std::move(new_elf));
        }

        // resolve all breakpoints in the target, as the user may have set
        // breakpoints that require the DWARF info of one of the shared libs to
        // resolve
        breakpoints_.for_each([&](auto& bp) { bp.resolve(); });
    }
}

void gsdb::target::notify_thread_lifecycle_event(const stop_reason& reason) {
    auto tid = reason.tid;
    if (reason.reason == process_state::stopped) {
        // if the stop reason is a signal, this is a new thread creation event
        auto& state = process_->thread_states()[tid];
        threads_.emplace(tid, thread{&state, stack{this, tid}});
    } else {
        // this is a thread exit event
        threads_.erase(tid);
    }
}
