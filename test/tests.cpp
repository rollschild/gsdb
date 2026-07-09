#include <elf.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <libgsdb/bit.hpp>
#include <libgsdb/dwarf.hpp>
#include <libgsdb/process.hpp>
#include <libgsdb/target.hpp>
#include <regex>
#include <string>
#include <vector>

#include "libgsdb/detail/dwarf.h"
#include "libgsdb/elf.hpp"
#include "libgsdb/error.hpp"
#include "libgsdb/pipe.hpp"
#include "libgsdb/register_info.hpp"
#include "libgsdb/syscalls.hpp"
#include "libgsdb/types.hpp"

using namespace gsdb;

namespace {
bool process_exists(pid_t pid) {
    // if `kill` is called with signal of `0`, it does not send a signal to the
    // process, but still carries out the existence and permissions checks
    auto ret = kill(pid, 0);
    // ESRCH: no such process
    // when PID is invalid or process has already terminated
    return ret != -1 and errno != ESRCH;
}

char get_process_status(pid_t pid) {
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string data;
    std::getline(stat, data);
    auto index_of_last_paren = data.rfind(')');
    auto index_of_status_indicator = index_of_last_paren + 2;
    return data[index_of_status_indicator];
}

std::int64_t get_section_load_bias(std::filesystem::path path,
                                   Elf64_Addr file_address) {
    // `readelf -WS <program name>`
    // Prints out the section headers with the entry for each section on a
    // single line
    auto command = std::string("readelf -WS ") + path.string();
    // execute the command, also
    // open a pipe from which we read its output
    auto pipe = popen(command.c_str(), "r");

    // (file address) (file offset) (size of the section)
    std::regex text_regex(R"(PROGBITS\s+(\w+)\s+(\w+)\s+(\w+))");
    char* line = nullptr;
    std::size_t len = 0;
    // Use the C version here, because C++ version operates on stream rather
    // than file descriptor
    // This function dynamically allocates a string that stores a line and
    // stores it in the first argument
    while (getline(&line, &len, pipe) != -1) {
        std::cmatch groups;
        if (std::regex_search(line, groups, text_regex)) {
            auto address = std::stoul(groups[1], nullptr, 16);
            auto offset = std::stoul(groups[2], nullptr, 16);
            auto size = std::stoul(groups[3], nullptr, 16);

            if (address <= file_address and file_address < (address + size)) {
                free(line);
                pclose(pipe);
                return address - offset;
            }
        }

        free(line);
        line = nullptr;
    }

    pclose(pipe);
    gsdb::error::send("Could not find section load bias!");
}

/* Computes the file offset of the binary's entry point (_start), i.e., where in
  the ELF file on disk the entry point's code lives. */
std::int64_t get_entry_point_offset(std::filesystem::path path) {
    std::ifstream elf_file(path);

    Elf64_Ehdr header;
    // Reads ELF header, which is always at the very start of the file.
    elf_file.read(reinterpret_cast<char*>(&header), sizeof(header));

    // entry point's file address
    // the virtual address the ELF assigns to `_start`
    auto entry_file_address = header.e_entry;
    // load bias: difference between a section's file address (virtual address
    // in ELF) and its file offset (position on disk)
    auto load_bias = get_section_load_bias(path, entry_file_address);
    // where **entry point** is on disk
    return entry_file_address - load_bias;
}

virt_addr get_load_address(pid_t pid, std::int64_t offset) {
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    // the `..(.).` is trying to match `r-xp`
    std::regex map_regex(R"((\w+)-\w+\s+..(.).\s+(\w+))");

    std::string data;
    while (std::getline(maps, data)) {
        std::smatch groups;
        std::regex_search(data, groups, map_regex);

        if (groups[2] == 'x') {
            auto low_range = std::stol(groups[1], nullptr, 16);
            auto file_offset = std::stol(groups[3], nullptr, 16);
            return virt_addr(offset - file_offset + low_range);
        }
    }

    gsdb::error::send("Could not find load address!");
}
}  // namespace

// [process] is a tag, so that you could run
// `./tests '[process]'`
TEST_CASE("process::launch success", "[process]") {
    auto proc = process::launch("sleep");
    REQUIRE(process_exists(proc->pid()));
}

TEST_CASE("process::launch no such program", "[process]") {
    REQUIRE_THROWS_AS(process::launch("non_exist_program"), error);
}

TEST_CASE("process::attach success", "[process]") {
    auto target =
        process::launch(std::string(TARGETS_DIR) + "/run_endlessly", false);
    auto proc = process::attach(target->pid());
    REQUIRE(get_process_status(target->pid()) == 't');
}

TEST_CASE("process::attach invalid PID", "[process]") {
    REQUIRE_THROWS_AS(process::attach(0), error);
}

TEST_CASE("process::resume success", "[process]") {
    {
        auto proc =
            process::launch(std::string(TARGETS_DIR) + "/run_endlessly");
        proc->resume();
        auto status = get_process_status(proc->pid());
        auto success = status == 'R' or status == 'S';
        REQUIRE(success);
    }
    {
        auto target =
            process::launch(std::string(TARGETS_DIR) + "/run_endlessly", false);
        auto proc = process::attach(target->pid());
        proc->resume();
        auto status = get_process_status(proc->pid());
        auto success = status == 'R' or status == 'S';
        REQUIRE(success);
    }
}

TEST_CASE("process::resume already terminated", "[process]") {
    auto proc = process::launch(std::string(TARGETS_DIR) + "/end_immediately");
    proc->resume();
    proc->wait_on_signal();  // wait for proc to terminate
    REQUIRE_THROWS_AS(proc->resume(), error);
}

TEST_CASE("Write register works", "[register]") {
    bool close_on_exec = false;
    gsdb::pipe channel(close_on_exec);

    auto proc = process::launch(std::string(TARGETS_DIR) + "/reg_write", true,
                                channel.get_write());
    channel.close_write();

    proc->resume();
    proc->wait_on_signal();

    auto& regs = proc->get_registers();
    regs.write_by_id(register_id::rsi, 0xcafecafe);

    proc->resume();  // resume again until next trap
    proc->wait_on_signal();

    auto output = channel.read();
    REQUIRE(to_string_view(output) == "0xcafecafe");

    regs.write_by_id(register_id::mm0, 0xdeadbeef);

    proc->resume();
    proc->wait_on_signal();

    output = channel.read();
    REQUIRE(to_string_view(output) == "0xdeadbeef");

    // test floating points
    regs.write_by_id(register_id::xmm0, 42.24);

    proc->resume();
    proc->wait_on_signal();

    output = channel.read();
    REQUIRE(to_string_view(output) == "42.24");

    regs.write_by_id(register_id::st0, 42.24l);
    // `fsw`: FPU status word, 16 bits wide
    //   - tracks current size of the FPU stack
    //   - reports errors like stack overflows, precision errors, or divisions
    //   by zero
    //   - bits 11 - 13 track top of the stack
    // `ftw`: FPU tag word
    //   - tracks which of the st registers are valid/empty/special (contain
    //   NaNs or infinity)
    //   - tag of `0b11` means empty
    //   - `0b00` means valid
    //
    // to push value to the stack, set bits 11 to 13 to 7 (0b111) - physical
    // register 7
    // The top starts at index 0 and goes down
    // A push decrements TOP. Starting from 0, one push wraps it to 7 (since 0 -
    // 1 mod 8 = 7)
    // So setting TOP=7 simulates the state _after_ one value has been pushed
    // onto a previously empty stack. The FPU sees "one push happened, st0 is at
    // R7, and it's valid"
    regs.write_by_id(register_id::fsw, std::uint16_t{0b0011100000000000});
    // set first tag to 0b00 and rest to 0b11
    regs.write_by_id(register_id::ftw, std::uint16_t{0b0011111111111111});

    proc->resume();
    proc->wait_on_signal();

    output = channel.read();
    REQUIRE(to_string_view(output) == "42.24");
}

TEST_CASE("Read register works", "[register]") {
    bool close_on_exec = false;
    gsdb::pipe channel(close_on_exec);

    auto proc = process::launch(std::string(TARGETS_DIR) + "/reg_read");
    auto& regs = proc->get_registers();

    proc->resume();  // resume again until next trap
    proc->wait_on_signal();

    REQUIRE(regs.read_by_id_as<std::uint64_t>(register_id::r13) == 0xcafecafe);

    proc->resume();
    proc->wait_on_signal();

    REQUIRE(regs.read_by_id_as<std::uint8_t>(register_id::r13b) == 42);

    proc->resume();
    proc->wait_on_signal();

    // NOTE the `ull` suffix to ensure the integer is `unsigned long long` -
    // `uint64_t`
    REQUIRE(regs.read_by_id_as<byte64>(register_id::mm0) ==
            to_byte64(0xdeadbeefull));

    proc->resume();
    proc->wait_on_signal();

    // Generally, you shouldn’t compare floating-point values by their bit
    // representations due to precision and rounding issues. However, binary can
    // exactly represent floating-point values whose denominators are powers of
    // two (such as 0.5, 0.25, 0.125, and so on), so it’s safe for us to do so
    // here.
    // We call such floating-point numbers **dyadic rationals**
    REQUIRE(regs.read_by_id_as<byte128>(register_id::xmm0) ==
            to_byte128(64.125));

    proc->resume();
    proc->wait_on_signal();

    // NOTE the `L` suffix - `long double`
    REQUIRE(regs.read_by_id_as<long double>(register_id::st0) == 64.125L);
}

TEST_CASE("Can create breakpoint site", "[breakpoint]") {
    auto proc = process::launch(std::string(TARGETS_DIR) + "/run_endlessly");
    auto& site = proc->create_breakpoint_site(virt_addr{42});
    REQUIRE(site.address().addr() == 42);
}

TEST_CASE("Breakpoint site IDs increase", "[breakpoint]") {
    auto proc = process::launch(std::string(TARGETS_DIR) + "/run_endlessly");

    auto& s1 = proc->create_breakpoint_site(virt_addr{42});
    REQUIRE(s1.address().addr() == 42);

    auto& s2 = proc->create_breakpoint_site(virt_addr{43});
    REQUIRE(s2.id() == s1.id() + 1);

    auto& s3 = proc->create_breakpoint_site(virt_addr{44});
    REQUIRE(s3.id() == s1.id() + 2);

    auto& s4 = proc->create_breakpoint_site(virt_addr{45});
    REQUIRE(s4.id() == s1.id() + 3);
}

TEST_CASE("Can find breakpoint site", "[breakpoint]") {
    auto proc = process::launch(std::string(TARGETS_DIR) + "/run_endlessly");
    const auto* cproc = proc.get();

    proc->create_breakpoint_site(virt_addr{42});
    proc->create_breakpoint_site(virt_addr{43});
    proc->create_breakpoint_site(virt_addr{44});
    proc->create_breakpoint_site(virt_addr{45});

    auto& s1 = proc->breakpoint_sites().get_by_address(virt_addr{44});
    REQUIRE(proc->breakpoint_sites().contains_address(virt_addr{44}));
    REQUIRE(s1.address().addr() == 44);

    auto& cs1 = cproc->breakpoint_sites().get_by_address(virt_addr{44});
    REQUIRE(cproc->breakpoint_sites().contains_address(virt_addr{44}));
    REQUIRE(cs1.address().addr() == 44);

    auto& s2 = proc->breakpoint_sites().get_by_id(s1.id() + 1);
    REQUIRE(proc->breakpoint_sites().contains_id(s1.id() + 1));
    REQUIRE(s2.address().addr() == 45);

    auto& cs2 = cproc->breakpoint_sites().get_by_id(cs1.id() + 1);
    REQUIRE(cproc->breakpoint_sites().contains_id(cs1.id() + 1));
    REQUIRE(cs2.address().addr() == 45);
    REQUIRE(cs2.id() == cs1.id() + 1);
}

TEST_CASE("Cannot find breakpoint site", "[breakpoint]") {
    auto proc = process::launch(std::string(TARGETS_DIR) + "/run_endlessly");
    const auto* cproc = proc.get();

    REQUIRE_THROWS_AS(proc->breakpoint_sites().get_by_address(virt_addr{44}),
                      error);
    REQUIRE_THROWS_AS(proc->breakpoint_sites().get_by_id(44), error);
    REQUIRE_THROWS_AS(cproc->breakpoint_sites().get_by_address(virt_addr{44}),
                      error);
    REQUIRE_THROWS_AS(cproc->breakpoint_sites().get_by_id(44), error);
}

TEST_CASE("Breakpoint site list size and emptiness", "[breakpoint]") {
    auto proc = process::launch(std::string(TARGETS_DIR) + "/run_endlessly");
    const auto* cproc = proc.get();

    REQUIRE(proc->breakpoint_sites().empty());
    REQUIRE(proc->breakpoint_sites().size() == 0);
    REQUIRE(cproc->breakpoint_sites().empty());
    REQUIRE(cproc->breakpoint_sites().size() == 0);

    proc->create_breakpoint_site(virt_addr{42});
    REQUIRE(!proc->breakpoint_sites().empty());
    REQUIRE(proc->breakpoint_sites().size() == 1);
    REQUIRE(!cproc->breakpoint_sites().empty());
    REQUIRE(cproc->breakpoint_sites().size() == 1);

    proc->create_breakpoint_site(virt_addr{43});
    REQUIRE(!proc->breakpoint_sites().empty());
    REQUIRE(proc->breakpoint_sites().size() == 2);
    REQUIRE(!cproc->breakpoint_sites().empty());
    REQUIRE(cproc->breakpoint_sites().size() == 2);
}

TEST_CASE("Can iterate breakpoint sites", "[breakpoint]") {
    auto proc = process::launch(std::string(TARGETS_DIR) + "/run_endlessly");
    const auto* cproc = proc.get();

    proc->create_breakpoint_site(virt_addr{42});
    proc->create_breakpoint_site(virt_addr{43});
    proc->create_breakpoint_site(virt_addr{44});
    proc->create_breakpoint_site(virt_addr{45});

    proc->breakpoint_sites().for_each(
        // init-captures
        // creates an capture with initial value 42
        // `mutable` enables us to change the value of this capture during
        // iteration; otherwise the capture would be declared `const`
        [addr = uint64_t{42}](auto& site) mutable {
            REQUIRE(site.address().addr() == addr++);
        });
    cproc->breakpoint_sites().for_each(
        // init-captures
        // creates an capture with initial value 42
        // `mutable` enables us to change the value of this capture during
        // iteration; otherwise the capture would be declared `const`
        [addr = uint64_t{42}](auto& site) mutable {
            REQUIRE(site.address().addr() == addr++);
        });
}

TEST_CASE("Breakpoint on address works", "[breakpoint]") {
    bool close_on_exec = false;
    gsdb::pipe channel(close_on_exec);

    auto proc = process::launch(std::string(TARGETS_DIR) + "/hello_gsdb", true,
                                channel.get_write());
    channel.close_write();

    auto offset =
        get_entry_point_offset(std::string(TARGETS_DIR) + "/hello_gsdb");
    auto load_address = get_load_address(proc->pid(), offset);

    proc->create_breakpoint_site(load_address).enable();
    proc->resume();
    auto reason = proc->wait_on_signal();

    REQUIRE(reason.reason == process_state::stopped);
    REQUIRE(reason.info == SIGTRAP);
    REQUIRE(proc->get_pc() == load_address);

    proc->resume();
    reason = proc->wait_on_signal();

    REQUIRE(reason.reason == process_state::exited);
    REQUIRE(reason.info == 0);

    auto data = channel.read();
    REQUIRE(to_string_view(data) == "Hello, gsdb!\n");
}

TEST_CASE("Can remove breakpoint sites", "[breakpoint]") {
    auto proc = process::launch(std::string(TARGETS_DIR) + "/run_endlessly");
    auto& site = proc->create_breakpoint_site(virt_addr{42});
    proc->create_breakpoint_site(virt_addr{43});
    REQUIRE(proc->breakpoint_sites().size() == 2);

    proc->breakpoint_sites().remove_by_id(site.id());
    proc->breakpoint_sites().remove_by_address(virt_addr{43});
    REQUIRE(proc->breakpoint_sites().empty());
}

TEST_CASE("Reading and writing memory works", "[memory]") {
    bool close_on_exec = false;
    gsdb::pipe channel(close_on_exec);
    auto proc = process::launch(std::string(TARGETS_DIR) + "/memory", true,
                                channel.get_write());
    channel.close_write();

    proc->resume();
    proc->wait_on_signal();

    auto a_pointer = from_bytes<std::uint64_t>(channel.read().data());
    auto data_vec = proc->read_memory(virt_addr{a_pointer}, 8);
    auto data = from_bytes<std::uint64_t>(data_vec.data());
    REQUIRE(data == 0xcafecafe);

    proc->resume();
    proc->wait_on_signal();

    auto b_pointer = from_bytes<std::uint64_t>(channel.read().data());
    proc->write_memory(virt_addr{b_pointer}, {as_bytes("Hello, gsdb!"), 13});

    proc->resume();
    proc->wait_on_signal();

    auto read = channel.read();
    REQUIRE(to_string_view(read) == "Hello, gsdb!");
}

TEST_CASE("Hardware breakpoint evades memory checksums", "[breakpoint]") {
    bool close_on_exec = false;
    gsdb::pipe channel(close_on_exec);
    auto proc = process::launch(std::string(TARGETS_DIR) + "/anti_debugger",
                                true, channel.get_write());
    channel.close_write();

    proc->resume();
    proc->wait_on_signal();

    // `channel.read().data()` reads the raw 8-byte address of
    // `an_innocent_function`, which is written by the `anti_debugger.cpp` main
    // function
    auto func = virt_addr(from_bytes<std::uint64_t>(channel.read().data()));
    auto& soft = proc->create_breakpoint_site(func, false);
    soft.enable();

    proc->resume();
    proc->wait_on_signal();

    // breakpoint already set
    REQUIRE(to_string_view(channel.read()) ==
            "NeoVim is even better than Vim...\n");

    proc->breakpoint_sites().remove_by_id(soft.id());
    auto& hard = proc->create_breakpoint_site(func, true);
    hard.enable();

    proc->resume();
    proc->wait_on_signal();

    // hardware breakpoint triggered
    // For hardware execute breakpoints, the CPU traps before executing the
    // instruction at the watched address, so PC is already at func — no rewind
    // needed
    REQUIRE(proc->get_pc() == func);

    proc->resume();
    proc->wait_on_signal();

    // hardware breakpoint set, but function code _NOT_ changed
    REQUIRE(to_string_view(channel.read()) == "Vim better than Emacs...\n");
}

/* Verifies that gsdb can defeat an anti-debugging
  checksum check by timing a software breakpoint insertion
  correctly — using a hardware watchpoint as the synchronization
   primitive */
TEST_CASE("Watchpoint detects read", "[watchpoint]") {
    bool close_on_exec = false;
    gsdb::pipe channel(close_on_exec);
    auto proc = process::launch(std::string(TARGETS_DIR) + "/anti_debugger",
                                true, channel.get_write());
    channel.close_write();

    proc->resume();
    proc->wait_on_signal();

    // where we want to eventually set a breakpoint
    auto func = virt_addr(from_bytes<std::uint64_t>(channel.read().data()));

    // set a watchpoint on that address so the program will halt when the
    // checksum funcion tries to read the code of the function on which we will
    // set a breakpoint
    auto& watch =
        proc->create_watchpoint(func, gsdb::stoppoint_mode::read_write, 1);
    watch.enable();

    // resume the process until the watch point is hit
    proc->resume();
    proc->wait_on_signal();

    // Step over a single instruction so we don't re-trip the same watchpoint
    proc->step_instruction();
    // Now insert a software breakpoint at func.
    // The int3 byte is written after the checksum has already
    // consumed the original byte, so the integrity check has already
    // passed
    auto& soft = proc->create_breakpoint_site(func, false);
    soft.enable();

    // resume the process - eventually the program calls func,
    // hits the `int3`
    // at this point, the software breakpoint should have been hit, so require
    // that the process stop due to SIGTRAP
    proc->resume();
    auto reason = proc->wait_on_signal();

    REQUIRE(reason.info == SIGTRAP);

    proc->resume();
    proc->wait_on_signal();

    REQUIRE(to_string_view(channel.read()) == "Vim better than Emacs...\n");
}

TEST_CASE("Syscall mapping works", "[syscall]") {
    REQUIRE(gsdb::syscall_id_to_name(0) == "read");
    REQUIRE(gsdb::syscall_name_to_id("read") == 0);
    REQUIRE(gsdb::syscall_id_to_name(62) == "kill");
    REQUIRE(gsdb::syscall_name_to_id("kill") == 62);
}

TEST_CASE("Syscall catchpoints work", "[catchpoint]") {
    auto dev_null = open("/dev/null", O_WRONLY);
    auto proc = process::launch(std::string(TARGETS_DIR) + "/anti_debugger",
                                true, dev_null);

    auto write_syscall = gsdb::syscall_name_to_id("write");
    auto policy = gsdb::syscall_catch_policy::catch_some({write_syscall});
    proc->set_syscall_catch_policy(policy);

    proc->resume();
    auto reason = proc->wait_on_signal();

    REQUIRE(reason.reason == gsdb::process_state::stopped);
    REQUIRE(reason.info == SIGTRAP);
    REQUIRE(reason.trap_reason == gsdb::trap_type::syscall);
    REQUIRE(reason.syscall_info->id == write_syscall);
    REQUIRE(reason.syscall_info->entry == true);

    proc->resume();
    reason = proc->wait_on_signal();

    REQUIRE(reason.reason == gsdb::process_state::stopped);
    REQUIRE(reason.info == SIGTRAP);
    REQUIRE(reason.trap_reason == gsdb::trap_type::syscall);
    REQUIRE(reason.syscall_info->id == write_syscall);
    REQUIRE(reason.syscall_info->entry == false);

    close(dev_null);
}

TEST_CASE("ELF parser works", "[elf]") {
    auto path = std::string(TARGETS_DIR) + "/hello_gsdb";
    gsdb::elf elf(path);
    auto entry = elf.get_header().e_entry;
    // Ensure that symbol at the entry point offset is the `_start` function.
    // Supplied by C++ toolchain, this function sets up the support that the
    // program needs to run before calling `main`.
    auto sym = elf.get_symbol_at_address(file_addr{elf, entry});
    auto name = elf.get_string(sym.value()->st_name);
    REQUIRE(name == "_start");

    auto syms = elf.get_symbols_by_name("_start");
    name = elf.get_string(syms.at(0)->st_name);
    REQUIRE(name == "_start");

    // Pretend ELF file is loaded at 0xcafecafe
    elf.notify_loaded(virt_addr{0xcafecafe});

    // Ensure that `0xcafecafe + entry` finds the `_start` symbol if we look up
    // the symbol by address rather than offset
    sym = elf.get_symbol_at_address(virt_addr{0xcafecafe + entry});
    name = elf.get_string(sym.value()->st_name);
    REQUIRE(name == "_start");
}

TEST_CASE("Correct DWARF language", "[dwarf]") {
    auto path = std::string(TARGETS_DIR) + "/hello_gsdb";
    gsdb::elf elf(path);

    auto& compile_units = elf.get_dwarf().compile_units();
    REQUIRE(compile_units.size() == 1);

    auto& cu = compile_units[0];
    auto lang = cu->root()[DW_AT_language].as_int();
    REQUIRE(lang == DW_LANG_C_plus_plus);
}

TEST_CASE("Iterate DWARF", "[dwarf]") {
    auto path = std::string(TARGETS_DIR) + "/hello_gsdb";
    gsdb::elf elf(path);

    auto& compile_units = elf.get_dwarf().compile_units();
    REQUIRE(compile_units.size() == 1);

    auto& cu = compile_units[0];
    std::size_t count = 0;
    for (auto& d : cu->root().children()) {
        auto a = d.abbrev_entry();
        REQUIRE(a->code != 0);
        ++count;
    }

    REQUIRE(count > 0);
}

TEST_CASE("Find main", "[dwarf]") {
    auto path = std::string(TARGETS_DIR) + "/hello_gsdb";
    gsdb::elf elf(path);
    gsdb::dwarf dwarf(elf);

    bool found = false;
    for (auto& cu : dwarf.compile_units()) {
        // loop through children of the root compile unit DIEs
        for (auto& die : cu->root().children()) {
            // represents a function AND has a DW_AT_name attribute whose
            // content is `main`
            if (die.abbrev_entry()->tag == DW_TAG_subprogram and
                die.contains(DW_AT_name)) {
                auto name = die[DW_AT_name].as_string();
                if (name == "main") {
                    found = true;
                }
            }
        }
    }

    REQUIRE(found);
}

TEST_CASE("Range list", "[dwarf]") {
    auto path = std::string(TARGETS_DIR) + "/hello_gsdb";
    gsdb::elf elf(path);
    gsdb::dwarf dwarf(elf);
    auto& cu = dwarf.compile_units()[0];

    // create range consists of a regular entry, a base address selector,
    // another regular entry, and an end-of-list indicator
    std::vector<std::uint64_t> range_data{0x12341234, 0x12341236, ~0ULL, 0x32,
                                          0x12341234, 0x12341236, 0x0,   0x0};

    auto bytes = reinterpret_cast<std::byte*>(range_data.data());
    gsdb::range_list list(cu.get(), {bytes, bytes + range_data.size()},
                          file_addr{});

    // The first entry should have the low address 0x12341234 and the high
    // address 0x12341236.
    auto it = list.begin();
    auto e1 = *it;
    REQUIRE(e1.low.addr() == 0x12341234);
    REQUIRE(e1.high.addr() == 0x12341236);
    REQUIRE(e1.contains(file_addr{elf, 0x12341234}));
    REQUIRE(e1.contains(file_addr{elf, 0x12341235}));
    REQUIRE(!e1.contains(file_addr{elf, 0x12341236}));

    // should be offset by 0x32, because of the base selector
    ++it;
    auto e2 = *it;
    REQUIRE(e2.low.addr() == 0x12341266);
    REQUIRE(e2.high.addr() == 0x12341268);
    REQUIRE(e2.contains(file_addr{elf, 0x12341266}));
    REQUIRE(e2.contains(file_addr{elf, 0x12341267}));
    REQUIRE(!e2.contains(file_addr{elf, 0x12341268}));

    ++it;
    REQUIRE(it == list.end());

    REQUIRE(list.contains(file_addr{elf, 0x12341234}));
    REQUIRE(list.contains(file_addr{elf, 0x12341235}));
    REQUIRE(!list.contains(file_addr{elf, 0x12341236}));
    REQUIRE(list.contains(file_addr{elf, 0x12341266}));
    REQUIRE(list.contains(file_addr{elf, 0x12341267}));
    REQUIRE(!list.contains(file_addr{elf, 0x12341268}));
}

/**
$ readelf
  --debug-dump=decodedline build/test/targets/hello_gsdb

  hello_gsdb.cpp  4  0x1139
  hello_gsdb.cpp  4  0x113d
  hello_gsdb.cpp  4  0x114c
  hello_gsdb.cpp  4  0x1151
  hello_gsdb.cpp  -  0x1153
*/
TEST_CASE("Line table", "[dwarf]") {
    auto path = std::string(TARGETS_DIR) + "/hello_gsdb";
    gsdb::elf elf(path);
    gsdb::dwarf dwarf(elf);

    REQUIRE(dwarf.compile_units().size() == 1);

    auto& cu = dwarf.compile_units()[0];
    auto it = cu->lines().begin();

    // line 3 is blank
    REQUIRE(it->line == 4);  // main function
    REQUIRE(it->file_entry->path.filename() == "hello_gsdb.cpp");

    // still line 4 since `std::puts` on same line with main
    ++it;
    REQUIRE(it->line == 4);

    ++it;
    REQUIRE(it->line == 4);

    ++it;
    REQUIRE(it->line == 4);

    ++it;
    REQUIRE(it->end_sequence);

    ++it;
    REQUIRE(it == cu->lines().end());
}

TEST_CASE("Source-level breakpoints", "[breakpoint]") {
    auto dev_null = open("/dev/null", O_WRONLY);
    auto path = std::string(TARGETS_DIR) + "/overloaded";
    auto target = target::launch(path, dev_null);
    auto& proc = target->get_process();

    target->create_line_breakpoint("overloaded.cpp", 17).enable();

    proc.resume();
    proc.wait_on_signal();

    auto entry = target->line_entry_at_pc();
    REQUIRE(entry->file_entry->path.filename() == "overloaded.cpp");
    REQUIRE(entry->line == 17);

    auto& bkpt = target->create_function_breakpoint("print_type");
    bkpt.enable();

    // disable the breakpoint site with the lowest address
    gsdb::breakpoint_site* lowest_bkpt = nullptr;
    bkpt.breakpoint_sites().for_each([&lowest_bkpt](auto& site) {
        if (lowest_bkpt == nullptr or
            site.address().addr() < lowest_bkpt->address().addr()) {
            lowest_bkpt = &site;
        }
    });
    lowest_bkpt->disable();

    proc.resume();
    proc.wait_on_signal();

    REQUIRE(target->line_entry_at_pc()->line == 9);

    proc.resume();
    proc.wait_on_signal();

    REQUIRE(target->line_entry_at_pc()->line == 13);

    proc.resume();
    auto reason = proc.wait_on_signal();

    REQUIRE(reason.reason == gsdb::process_state::exited);
    close(dev_null);
}

TEST_CASE("Source-level stepping", "[target]") {
    auto dev_null = open("/dev/null", O_WRONLY);
    auto path = std::string(TARGETS_DIR) + "/step";
    auto target = target::launch(path, dev_null);
    auto& proc = target->get_process();

    target->create_function_breakpoint("main").enable();
    proc.resume();
    proc.wait_on_signal();

    auto pc = proc.get_pc();
    REQUIRE(target->function_name_at_address(pc) == "main");

    // step over first call to find_happiness
    target->step_over();

    auto new_pc = proc.get_pc();
    REQUIRE(new_pc != pc);
    // make sure still inside main
    REQUIRE(target->function_name_at_address(pc) == "main");

    // step into find_happiness
    target->step_in();

    pc = proc.get_pc();
    REQUIRE(target->function_name_at_address(pc) == "find_happiness");
    // we’re also at the top of the inlined pet_cat and scratch_head functions
    REQUIRE(target->get_stack().inline_height() == 2);

    target->step_in();

    new_pc = proc.get_pc();
    REQUIRE(new_pc == pc);
    // step into pet_cat
    // inline height has gone down
    REQUIRE(target->get_stack().inline_height() == 1);

    target->step_out();

    new_pc = proc.get_pc();
    REQUIRE(new_pc != pc);
    REQUIRE(target->function_name_at_address(pc) == "find_happiness");

    target->step_out();

    pc = proc.get_pc();
    REQUIRE(target->function_name_at_address(pc) == "main");

    close(dev_null);
}
