#include <libgsdb/breakpoint.hpp>
#include <libgsdb/target.hpp>
#include <string>
#include <utility>

#include "libgsdb/detail/dwarf.h"
#include "libgsdb/types.hpp"

namespace {
auto get_next_id() {
    static gsdb::breakpoint::id_type id = 0;
    return ++id;
}
}  // namespace

gsdb::breakpoint::breakpoint(target& target, bool is_hardware, bool is_internal)
    : target_{&target}, is_hardware_{is_hardware}, is_internal_{is_internal} {
    id_ = is_internal ? -1 : get_next_id();
}

void gsdb::breakpoint::enable() {
    is_enabled_ = true;
    breakpoint_sites_.for_each([](auto& site) { site.enable(); });
}
void gsdb::breakpoint::disable() {
    is_enabled_ = false;
    breakpoint_sites_.for_each([](auto& site) { site.disable(); });
}

void gsdb::address_breakpoint::resolve() {
    if (breakpoint_sites_.empty()) {
        auto& new_site = target_->get_process().create_breakpoint_site(
            this, next_site_id_++, address_, is_hardware_, is_internal_);
        breakpoint_sites_.push(&new_site);
        if (is_enabled_) {
            new_site.enable();
        }
    }
}

void gsdb::function_breakpoint::resolve() {
    auto found_functions = target_->find_functions(function_name_);
    // loop over all DIEs matching the given name
    for (auto die : found_functions.dwarf_functions) {
        // If we can retrieve the function’s start address, we try to resolve
        // the first instruction after the prologue.
        if (die.contains(DW_AT_low_pc) or die.contains(DW_AT_ranges)) {
            file_addr addr;
            if (die.abbrev_entry()->tag == DW_TAG_inlined_subroutine) {
                // inlined, no prologue
                // set a breakpoint at the start of the function
                addr = die.low_pc();
            } else {
                // find the corresponding line entry for the start of the
                // function and then advance the resulting iterator to find the
                // entry pointing to the first instruction after the prologue
                auto function_line =
                    die.cu()->lines().get_entry_by_address(die.low_pc());
                ++function_line;
                addr = function_line->address;
            }
            // Calculate where the instruction was loaded,
            // because the address we just worked out is a file address
            auto load_address = addr.to_virt_addr();
            if (!breakpoint_sites_.contains_address(load_address)) {
                auto& new_site = target_->get_process().create_breakpoint_site(
                    this, next_site_id_++, load_address, is_hardware_,
                    is_internal_);
                breakpoint_sites_.push(&new_site);
                if (is_enabled_) new_site.enable();
            }
        }
    }

    for (auto sym : found_functions.elf_functions) {
        auto file_address = file_addr{*sym.first, sym.second->st_value};
        auto load_address = file_address.to_virt_addr();
        if (!breakpoint_sites_.contains_address(load_address)) {
            auto& new_site = target_->get_process().create_breakpoint_site(
                this, next_site_id_++, load_address, is_hardware_,
                is_internal_);
            breakpoint_sites_.push(&new_site);
            if (is_enabled_) new_site.enable();
        }
    }
}

/**
 * Find the memory address of the instruction corresponding to the given line,
 * potentially skip over a function prologue, and then
 * create a breakpoint site at the correct load address
 */
void gsdb::line_breakpoint::resolve() {
    auto& dwarf = target_->get_elf().get_dwarf();
    // the line we are looking for could be in any compile unit
    for (auto& cu : dwarf.compile_units()) {
        auto entries = cu->lines().get_entries_by_line(file_, line_);

        // _multiple_ entries could be associated with a single line of code
        // so we resolve breakpoint sites for all entries
        for (auto entry : entries) {
            // Grab the DWARF file from the line table entry rather than using
            // the one we got from the target. This is to support shared libs
            auto& dwarf = entry->address.elf_file()->get_dwarf();
            auto stack = dwarf.inline_stack_at_address(entry->address);
            auto no_inline_stack = stack.size() == 1;
            // Should skip prologue if:
            //   - no inline stack
            //   - the function DIE has address range information
            //   - address for the line table entry we found is at the start of
            //   the function
            auto should_skip_prologue = no_inline_stack and
                                        (stack[0].contains(DW_AT_ranges) or
                                         stack[0].contains(DW_AT_low_pc)) and
                                        stack[0].low_pc() == entry->address;
            if (should_skip_prologue) {
                ++entry;
            }
            auto load_address = entry->address.to_virt_addr();
            if (!breakpoint_sites_.contains_address(load_address)) {
                auto& new_site = target_->get_process().create_breakpoint_site(
                    this, next_site_id_++, load_address, is_hardware_,
                    is_internal_);
                breakpoint_sites_.push(&new_site);
                if (is_enabled_) new_site.enable();
            }
        }
    }
}
