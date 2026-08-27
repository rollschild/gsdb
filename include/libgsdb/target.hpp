#ifndef GSDB_TARGET_HPP
#define GSDB_TARGET_HPP

#include <elf.h>
#include <link.h>
#include <sys/types.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <libgsdb/elf.hpp>
#include <libgsdb/process.hpp>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "libgsdb/breakpoint.hpp"
#include "libgsdb/dwarf.hpp"
#include "libgsdb/stack.hpp"
#include "libgsdb/stoppoint_collection.hpp"
#include "libgsdb/types.hpp"

namespace gsdb {
struct thread {
    thread(thread_state* state, stack frames)
        : state(state), frames(std::move(frames)) {}

    thread_state* state;
    stack frames;
};
/**
 * Manage the symbolic level of the program that we’re debugging, such as
 * storing the `gsdb::elf` object for the program, reading debug information,
 * and carrying out debugger operations at the level of the source code.
 */
class target {
   public:
    target() = delete;
    target(const target&) = delete;
    target& operator=(const target&) = delete;

    /**
     * Parse the ELF file for the relevant process and set the load address of
     * the `.text` section
     */
    static std::unique_ptr<target> launch(
        std::filesystem::path path,
        std::optional<int> stdout_replacement = std::nullopt);
    static std::unique_ptr<target> attach(pid_t pid);

    process& get_process() { return *process_; }
    const process& get_process() const { return *process_; }
    elf& get_elf() { return *main_elf_; }
    const elf& get_elf() const { return *main_elf_; }

    void notify_stop(const gsdb::stop_reason& reason);

    /**
     * Convert the program counter from virtual address to file address
     */
    file_addr get_pc_file_address() const;

    // stack& get_stack() { return stack_; }
    // const stack& get_stack() const { return stack_; }

    stop_reason step_in();
    stop_reason step_out();
    stop_reason step_over();

    gsdb::line_table::iterator line_entry_at_pc() const;
    gsdb::stop_reason run_until_address(virt_addr address);

    struct find_functions_result {
        std::vector<die> dwarf_functions;
        std::vector<std::pair<const elf*, const Elf64_Sym*>> elf_functions;
    };
    /**
     * Return a result that wraps the DWARF DIEs and ELF symbols found for the
     * given name
     */
    find_functions_result find_functions(std::string name) const;

    breakpoint& create_address_breakpoint(virt_addr address,
                                          bool hardware = false,
                                          bool internal = false);
    breakpoint& create_function_breakpoint(std::string function_name,
                                           bool hardware = false,
                                           bool internal = false);
    breakpoint& create_line_breakpoint(std::filesystem::path file,
                                       std::size_t line, bool hardware = false,
                                       bool internal = false);

    stoppoint_collection<breakpoint>& breakpoints() { return breakpoints_; }
    const stoppoint_collection<breakpoint>& breakpoints() const {
        return breakpoints_;
    }

    std::string function_name_at_address(virt_addr address) const;

    std::optional<r_debug> read_dynamic_linker_rendezvous() const;

    elf_collection& get_elves() { return elves_; }
    const elf_collection& get_elves() const { return elves_; }
    elf& get_main_elf() { return *main_elf_; }
    const elf& get_main_elf() const { return *main_elf_; }

    std::vector<line_table::iterator> get_line_entries_by_line(
        std::filesystem::path path, std::size_t line) const;

    std::unordered_map<pid_t, thread>& threads() { return threads_; }
    const std::unordered_map<pid_t, thread>& threads() const {
        return threads_;
    }

    /**
     * Notify the target that a new thread has been created/terminated
     */
    void notify_thread_lifecycle_event(const stop_reason& reason);

   private:
    target(std::unique_ptr<process> proc, std::unique_ptr<elf> obj)
        : process_(std::move(proc)), main_elf_(obj.get()) {
        elves_.push(std::move(obj));
        auto pid = process_->pid();
        for (auto& [tid, state] : process_->thread_states()) {
            threads_.emplace(tid, thread(&state, stack{this, tid}));
        }
    }

    /**
     * Locate the dynamic section of the inferior, read it;
     * Locate the dynamic linker rendezvous structure address in the `DT_DEBUG`
     * entry, read the list of loaded dynamic libraries, and then set an
     * internal breakpoint on the `_dl_debug_state` function.
     */
    void resolve_dynamic_linker_rendezvous();
    void reload_dynamic_libraries();

    std::unique_ptr<process> process_;

    // stack stack_;

    stoppoint_collection<breakpoint> breakpoints_;

    virt_addr dynamic_linker_rendezvous_address_;

    elf_collection elves_;
    elf* main_elf_;

    std::unordered_map<pid_t, thread> threads_;
};
}  // namespace gsdb

#endif
