#include <editline/readline.h>
#include <elf.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <concepts>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <iterator>
#include <memory>
#include <print>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "libgsdb/breakpoint_site.hpp"
#include "libgsdb/disassembler.hpp"
#include "libgsdb/error.hpp"
#include "libgsdb/parse.hpp"
#include "libgsdb/process.hpp"
#include "libgsdb/register_info.hpp"
#include "libgsdb/registers.hpp"
#include "libgsdb/syscalls.hpp"
#include "libgsdb/target.hpp"
#include "libgsdb/types.hpp"
#include "libgsdb/watchpoint.hpp"

namespace {

// this exists precisely because you have no way to pass custom data to a signal
// handler
gsdb::process* g_gsdb_process = nullptr;

void handle_sigint(int) {
    // `kill` is async-signal-safe
    kill(g_gsdb_process->pid(), SIGSTOP);
}

[[maybe_unused]]
bool is_prefix(std::string_view str, std::string_view of) {
    if (str.size() > of.size()) return false;
    return std::equal(str.begin(), str.end(), of.begin());
}

void print_help(const std::vector<std::string>& args) {
    if (args.size() == 1) {
        // raw string literal
        std::cerr << R"(Available commands:
breakpoint  - Commands for operating on breakpoints
continue    - Resume the process
disassemble - Disassemble machine code to assembly
memory      - Commands for operating on memory
register    - Commands for operating on registers
step        - Step over a single instruction
watchpoint  - Commands for operating on watchpoints
catchpoint  - Commands for operating on catchpoints
)";
    } else if (is_prefix(args[1], "register")) {
        std::cerr << R"(Available commands:
read 
read <register>
read all
write <register> <value>
)";
    } else if (is_prefix(args[1], "breakpoint")) {
        std::cerr << R"(Available commands:
list
delete <id>
disable <id>
enable <id>
set <address>
set <address> -h
)";
    } else if (is_prefix(args[1], "watchpoint")) {
        std::cerr << R"(Available commands:
list
delete <id>
disable <id>
enable <id>
set <address> <write|rw|execute> <size>
)";
    } else if (is_prefix(args[1], "catchpoint")) {
        std::cerr << R"(Available commands:
syscall
syscall none
syscall <list-of-comma-separated-syscalls> 
)";
    } else if (is_prefix(args[1], "memory")) {
        std::cerr << R"(Available commands:
read <address>
read <address> <number-of-bytes>
write <address> <bytes>
)";
    } else if (is_prefix(args[1], "disassemble")) {
        std::cerr << R"(Available options:
-c <number-of-instructions>
-a <start-address>
)";
    } else {
        std::cerr << "No help available on that\n";
    }
}

/*
./build/tools/gsdb ./build/test/targets/hello_gsdb
Launched process with PID 33042
gsdb> break set 0x555555555147
gsdb> c
Process 33042 stopped with signal TRAP at 0x555555555147
0x0000555555555147: call 0x0000555555555030
0x000055555555514c: mov $0x00, %eax
0x0000555555555151: pop %rbp
0x0000555555555152: ret
0x0000555555555153: add %dh, %bl
gsdb>
*/
void print_disassembly(gsdb::process& process, gsdb::virt_addr address,
                       std::size_t n_instructions) {
    gsdb::disassembler dis(process);
    auto instructions = dis.disassemble(n_instructions, address);
    for (auto& instr : instructions) {
        std::print("{:#018x}: {}\n", instr.address.addr(), instr.text);
    }
}

/**
 * Launches, attaches to the given program name or PID.
 * Returns the PID of the inferior.
 */
std::unique_ptr<gsdb::target> attach(int argc, const char** argv) {
    // passing PID
    if (argc == 3 && argv[1] == std::string_view("-p")) {
        // std::string() would have dynamically allocated memory
        pid_t pid = std::atoi(argv[2]);
        return gsdb::target::attach(pid);
    } else {
        // passing program name
        auto program_path = argv[1];
        auto target = gsdb::target::launch(program_path);
        std::print("Launched process with PID {}\n",
                   target->get_process().pid());
        return target;
    }
}

std::vector<std::string> split(std::string_view str, char delimiter) {
    std::vector<std::string> out{};
    std::stringstream ss{std::string{str}};
    std::string item;

    while (std::getline(ss, item, delimiter)) {
        out.push_back(item);
    }

    return out;
}

template <std::input_iterator It, std::sentinel_for<It> S>
std::string format_join(It first, S last, std::string_view separator,
                        std::string_view byte_fmt = "{:#04x}") {
    std::string res;
    std::string_view sep = "";
    for (; first != last; ++first) {
        res += sep;
        if constexpr (std::same_as<std::iter_value_t<It>, std::byte>) {
            auto val = std::to_integer<std::uint8_t>(*first);
            /*
            std::format_to(std::back_inserter(res), "{}{:#04x}", sep,
                           std::to_integer<std::uint8_t>(*first));
            */
            std::vformat_to(std::back_inserter(res), byte_fmt,
                            std::make_format_args(val));

        } else {
            const auto& val = *first;
            std::vformat_to(std::back_inserter(res), byte_fmt,
                            std::make_format_args(val));
        }
        sep = separator;
    }
    return res;
}

template <std::ranges::range T>
// requires std::formattable<std::ranges::range_value_t<T>, char>
std::string format_join(const T& t, std::string_view separator) {
    return format_join(std::ranges::begin(t), std::ranges::end(t), separator);
    /* std::string res;
    std::string_view sep = "";
    for (const auto& elem : t) {
        if constexpr (std::same_as<std::ranges::range_value_t<T>, std::byte>) {
            std::format_to(std::back_inserter(res), "{}{:#04x}", sep,
                           std::to_integer<std::uint8_t>(elem));

        } else {
            std::format_to(std::back_inserter(res), "{}{}", sep, elem);
        }
        sep = separator;
    }
    return res; */
}

std::string get_sigtrap_info(const gsdb::process& process,
                             gsdb::stop_reason reason) {
    if (reason.trap_reason == gsdb::trap_type::software_break) {
        auto& site =
            process.breakpoint_sites().get_by_address(process.get_pc());
        return std::format(" (breakpoint {})", site.id());
    }

    if (reason.trap_reason == gsdb::trap_type::hardware_break) {
        auto id = process.get_current_hardware_stoppoint();
        if (id.index() == 0) {
            // hardware stoppoint, not watchpoint
            return std::format(" (breakpoint {})", std::get<0>(id));
        }

        std::string message;
        auto& point = process.watchpoints().get_by_id(std::get<1>(id));
        message += std::format(" (watchpoint {})", point.id());

        if (point.data() == point.previous_data()) {
            message += std::format("\nValue: {:#x}", point.data());
        } else {
            message += std::format("\nOld value: {:#x}\nNew value: {:#x}",
                                   point.previous_data(), point.data());
        }
        return message;
    }

    if (reason.trap_reason == gsdb::trap_type::single_step) {
        return " (single step)";
    }

    if (reason.trap_reason == gsdb::trap_type::syscall) {
        const auto& info = *(reason.syscall_info);
        std::string message = " ";
        if (info.entry) {
            message += "(syscall entry)\n";
            message += std::format(
                "syscall: {}({})", gsdb::syscall_id_to_name(info.id),
                format_join(std::begin(info.args), std::end(info.args), ",",
                            "{:#x}"));
        } else {
            message += "(syscall exit)\n";
            message += std::format("syscall returned: {:#x}", info.ret);
        }
        return message;
    }

    return "";
}

std::string get_signal_stop_reason(const gsdb::target& target,
                                   gsdb::stop_reason reason) {
    auto& process = target.get_process();
    std::string message =
        std::format("stopped with signal {} at {:#x}",
                    sigabbrev_np(reason.info), process.get_pc().addr());

    // pointer to the symbol corresponding to the current function
    auto func =
        target.get_elf().get_symbol_containing_address(process.get_pc());
    if (func and ELF64_ST_TYPE(func.value()->st_info) == STT_FUNC) {
        // if the symbol represents a function
        message += std::format(
            " ({})", target.get_elf().get_string(func.value()->st_name));
    }

    if (reason.info == SIGTRAP) {
        message += get_sigtrap_info(process, reason);
    }

    return message;
}

void print_stop_reason(const gsdb::target& target, gsdb::stop_reason reason) {
    // std::cout << "Process " << process.pid() << ' ';
    std::string message;

    switch (reason.reason) {
        case gsdb::process_state::exited:
            message = std::format("exited with status {}",
                                  static_cast<int>(reason.info));
            break;
        case gsdb::process_state::terminated:
            message = std::format("terminated with signal {}",
                                  sigabbrev_np(reason.info));
            break;
        case gsdb::process_state::stopped:
            message = get_signal_stop_reason(target, reason);
            break;
        default:
            message = "still running!";
            break;
    }

    std::print("Process {} {}\n", target.get_process().pid(), message);
}

void handle_register_read(gsdb::process& process,
                          const std::vector<std::string>& args) {
    auto format = [](auto t) {
        if constexpr (std::is_floating_point_v<decltype(t)>) {
            return std::format("{}", t);
        } else if constexpr (std::is_integral_v<decltype(t)>) {
            // nested `{}`: amount of padding to use
            // add 2 chars per byte of the integer, plus 2 chars for `0x`
            return std::format("{:#0{}x}", t, sizeof(t) * 2 + 2);
        } else if constexpr (std::ranges::range<decltype(t)>) {  // for vectors
            // format internal bytes as hex with leading `0x`, padded to 4
            // chars
            return std::format("[{}]", format_join(t, ","));
        } else {
            return std::string{"<unknown>"};
        }
    };

    // `register read` or `register read all`
    if (args.size() == 2 or (args.size() == 3 and args[2] == "all")) {
        for (auto& info : gsdb::g_register_infos) {
            auto should_print =
                (args.size() == 3 or info.type == gsdb::register_type::gpr) and
                info.name != "orig_rax";
            // orig_rax is not a real register, just something that `ptrace`
            // uses to communicate information about syscalls
            if (!should_print) continue;

            auto value = process.get_registers().read(info);
            std::print("{}:\t{}\n", info.name, std::visit(format, value));
        }
    } else if (args.size() == 3) {
        // `register read <register-name>`
        try {
            auto info = gsdb::register_info_by_name(args[2]);
            auto value = process.get_registers().read(info);
            std::print("{}:\t{}\n", info.name, std::visit(format, value));
        } catch (gsdb::error& err) {
            std::cerr << "No such register\n";
            return;
        }
    } else {
        print_help({"help", "register"});
    }
}

gsdb::registers::value parse_register_value(gsdb::register_info info,
                                            std::string_view text) {
    try {
        if (info.format == gsdb::register_format::uint) {
            switch (info.size) {
                case 1:
                    return gsdb::to_integral<std::uint8_t>(text, 16).value();
                case 2:
                    return gsdb::to_integral<std::uint16_t>(text, 16).value();
                case 4:
                    return gsdb::to_integral<std::uint32_t>(text, 16).value();
                case 8:
                    return gsdb::to_integral<std::uint64_t>(text, 16).value();
            }
        } else if (info.format == gsdb::register_format::double_float) {
            return gsdb::to_float<double>(text).value();
        } else if (info.format == gsdb::register_format::long_double) {
            return gsdb::to_float<long double>(text).value();
        } else if (info.format == gsdb::register_format::vector) {
            if (info.size == 8) {
                return gsdb::parse_vector<8>(text);
            } else if (info.size == 16) {
                return gsdb::parse_vector<16>(text);
            }
        }
    } catch (...) {
    }
    gsdb::error::send("Invalid format!");
}

void handle_register_write(gsdb::process& process,
                           const std::vector<std::string>& args) {
    if (args.size() != 4) {
        print_help({"help", "register"});
        return;
    }

    try {
        auto info = gsdb::register_info_by_name(args[2]);
        auto value = parse_register_value(info, args[3]);
        process.get_registers().write(info, value);
    } catch (gsdb::error& err) {
        std::cerr << err.what() << '\n';
        return;
    }
}

void handle_register_command(gsdb::process& process,
                             const std::vector<std::string>& args) {
    if (args.size() < 2) {
        print_help({"help", "register"});
        return;
    }

    if (is_prefix(args[1], "read")) {
        handle_register_read(process, args);
    } else if (is_prefix(args[1], "write")) {
        handle_register_write(process, args);
    } else {
        print_help({"help", "register"});
    }
}

void handle_breakpoint_command(gsdb::process& process,
                               const std::vector<std::string>& args) {
    if (args.size() < 2) {
        print_help({"help", "breakpoint"});
        return;
    }

    auto command = args[1];
    if (is_prefix(command, "list")) {
        // breakpoint list
        if (process.breakpoint_sites().empty()) {
            std::print("No breakpoints set\n");
        } else {
            std::print("Current breakpoints:\n");
            process.breakpoint_sites().for_each([](auto& site) {
                if (site.is_internal()) return;
                std::print("{}: address = {:#x}, {}\n", site.id(),
                           site.address().addr(),
                           site.is_enabled() ? "enabled" : "disabled");
            });
        }
        return;
    }

    if (args.size() < 3) {
        print_help({"help", "breakpoint"});
        return;
    }
    if (is_prefix(command, "set")) {
        auto address = gsdb::to_integral<std::uint64_t>(args[2], 16);
        if (!address) {
            std::print(stderr,
                       "Breakpoint command expects address in hexadecimal, "
                       "prefixed with '0x'\n");
            return;
        }

        bool hardware = false;
        if (args.size() == 4) {
            if (args[3] == "-h")
                hardware = true;
            else
                gsdb::error::send("Invalid breakpoint command argument!");
        }

        process.create_breakpoint_site(gsdb::virt_addr{*address}, hardware)
            .enable();
        return;
    }

    auto id = gsdb::to_integral<gsdb::breakpoint_site::id_type>(args[2]);
    if (!id) {
        std::cerr << "Command expects breakpoint ID!";
        return;
    }

    if (is_prefix(command, "enable")) {
        process.breakpoint_sites().get_by_id(*id).enable();
    } else if (is_prefix(command, "disable")) {
        process.breakpoint_sites().get_by_id(*id).disable();
    } else if (is_prefix(command, "delete")) {
        process.breakpoint_sites().remove_by_id(*id);
    }
}

void handle_watchpoint_list(
    gsdb::process& process,
    [[maybe_unused]] const std::vector<std::string>& args) {
    auto stoppoint_mode_to_string = [](auto mode) {
        switch (mode) {
            case gsdb::stoppoint_mode::execute:
                return "execute";
            case gsdb::stoppoint_mode::write:
                return "write";
            case gsdb::stoppoint_mode::read_write:
                return "read_write";
            default:
                gsdb::error::send("Invalid stoppoint mode!");
        }
    };

    if (process.watchpoints().empty()) {
        std::print("No watchpoints set!\n");
    } else {
        std::print("Current watchpoints:\n");
        process.watchpoints().for_each([&](auto& point) {
            std::print("{}: address = {:#x}, mode = {}, size = {}, {}\n",
                       point.id(), point.address().addr(),
                       stoppoint_mode_to_string(point.mode()), point.size(),
                       point.is_enabled() ? "enabled" : "disabled");
        });
    }
}

// watchpoint set <address> <mode> <size>
void handle_watchpoint_set(gsdb::process& process,
                           const std::vector<std::string>& args) {
    if (args.size() != 5) {
        print_help({"help", "watchpoint"});
        return;
    }
    auto address = gsdb::to_integral<std::uint64_t>(args[2], 16);
    auto mode_text = args[3];
    auto size = gsdb::to_integral<std::size_t>(args[4]);

    if (!address or !size or
        !(mode_text == "write" or mode_text == "rw" or
          mode_text == "execute")) {
        print_help({"help", "watchpoint"});
        return;
    }

    gsdb::stoppoint_mode mode;
    mode = mode_text == "write" ? gsdb::stoppoint_mode::write
           : mode_text == "rw"  ? gsdb::stoppoint_mode::read_write
                                : gsdb::stoppoint_mode::execute;

    process.create_watchpoint(gsdb::virt_addr{*address}, mode, *size).enable();
}

/**
 * Support:
 *   watchpoint list
 *   watchpoint set <address> <mode> <size>
 *   watchpoint enable <id>
 *   watchpoint disable <id>
 *   watchpoint delete <id>
 */
void handle_watchpoint_command(gsdb::process& process,
                               const std::vector<std::string>& args) {
    if (args.size() < 2) {
        print_help({"help", "watchpoint"});
        return;
    }

    auto command = args[1];

    if (is_prefix(command, "list")) {
        handle_watchpoint_list(process, args);
        return;
    }

    if (is_prefix(command, "set")) {
        handle_watchpoint_set(process, args);
        return;
    }

    if (args.size() < 3) {
        print_help({"help", "watchpoint"});
        return;
    }

    auto id = gsdb::to_integral<gsdb::watchpoint::id_type>(args[2]);
    if (!id) {
        std::cerr << "Command expects watchpoint ID!\n";
        return;
    }

    if (is_prefix(command, "enable")) {
        process.watchpoints().get_by_id(*id).enable();
    } else if (is_prefix(command, "disable")) {
        process.watchpoints().get_by_id(*id).disable();
    } else if (is_prefix(command, "delete")) {
        process.watchpoints().remove_by_id(*id);
    }
}

void handle_memory_read_command(gsdb::process& process,
                                const std::vector<std::string>& args) {
    auto address = gsdb::to_integral<std::uint64_t>(args[2], 16);
    if (!address) gsdb::error::send("Invalid address format!");

    auto n_bytes = 32;
    if (args.size() == 4) {
        auto bytes_arg = gsdb::to_integral<std::size_t>(args[3]);
        if (!bytes_arg) gsdb::error::send("Invalid number of bytes!");
        n_bytes = *bytes_arg;
    }

    auto data = process.read_memory(gsdb::virt_addr{*address}, n_bytes);

    // print out the memory
    for (std::size_t i = 0; i < data.size(); i += 16) {
        auto start = data.begin() + i;
        auto end = data.begin() + std::min(i + 16, data.size());
        std::print("{:#016x}: {}\n", *address + i,
                   format_join(start, end, " ", "{:02x}"));
    }
}

void handle_memory_write_command(gsdb::process& process,
                                 const std::vector<std::string>& args) {
    if (args.size() != 4) {
        print_help({"help", "memory"});
        return;
    }

    auto address = gsdb::to_integral<std::uint64_t>(args[2], 16);
    if (!address) gsdb::error::send("Invalid address format!");

    auto data = gsdb::parse_vector(args[3]);
    process.write_memory(gsdb::virt_addr{*address}, {data.data(), data.size()});
}

/**
 * Supports:
 *  - memory read <address>
 *  - memory read <address> <number of bytes>
 *  - memory write <address> <values>
 */
void handle_memory_command(gsdb::process& process,
                           const std::vector<std::string>& args) {
    if (args.size() < 3) {
        print_help({"help", "memory"});
        return;
    }

    if (is_prefix(args[1], "read")) {
        handle_memory_read_command(process, args);
    } else if (is_prefix(args[1], "write")) {
        handle_memory_write_command(process, args);
    } else {
        print_help({"help", "memory"});
    }
}

void handle_stop(gsdb::target& target, gsdb::stop_reason reason) {
    print_stop_reason(target, reason);
    if (reason.reason == gsdb::process_state::stopped) {
        print_disassembly(target.get_process(), target.get_process().get_pc(),
                          5);
    }
}
/**
 * `disassemble -c <n_instructions> -a <address>`
 * Default to 5 instructions and the current program counter value
 */
void handle_disassemble_command(gsdb::process& process,
                                const std::vector<std::string>& args) {
    auto address = process.get_pc();
    std::size_t n_instructions = 5;
    auto it = args.begin() + 1;
    while (it != args.end()) {
        if (*it == "-a" and it + 1 != args.end()) {
            ++it;
            auto opt_addr = gsdb::to_integral<std::uint64_t>(*it++, 16);
            if (!opt_addr) gsdb::error::send("Invalid address format!");
            address = gsdb::virt_addr{*opt_addr};
        } else if (*it == "-c" and it + 1 != args.end()) {
            ++it;
            auto opt_n = gsdb::to_integral<std::size_t>(*it++);
            if (!opt_n) gsdb::error::send("Invalid instruction count!");
            n_instructions = *opt_n;
        } else {
            print_help({"help", "disassemble"});
            return;
        }
    }

    print_disassembly(process, address, n_instructions);
}

/**
 * Support:
 *   `catchpoint syscall`
 *   `catchpoint syscall none`
 *   `catchpoint syscall <list-of-comma-separated-syscalls>`
 */
void handle_syscall_catchpoint_command(gsdb::process& process,
                                       const std::vector<std::string>& args) {
    gsdb::syscall_catch_policy policy = gsdb::syscall_catch_policy::catch_all();
    if (args.size() == 3 and args[2] == "none") {
        policy = gsdb::syscall_catch_policy::catch_none();
    } else if (args.size() >= 3) {
        auto syscalls = split(args[2], ',');
        std::vector<int> to_catch;
        std::transform(std::begin(syscalls), std::end(syscalls),
                       std::back_inserter(to_catch), [](auto& syscall) {
                           return isdigit(syscall[0])
                                      ? gsdb::to_integral<int>(syscall).value()
                                      : gsdb::syscall_name_to_id(syscall);
                       });
        policy = gsdb::syscall_catch_policy::catch_some(std::move(to_catch));
    }

    process.set_syscall_catch_policy(std::move(policy));
}

void handle_catchpoint_command(gsdb::process& process,
                               const std::vector<std::string>& args) {
    if (args.size() < 2) {
        print_help({"help", "catchpoint"});
        return;
    }

    if (is_prefix(args[1], "syscall")) {
        handle_syscall_catchpoint_command(process, args);
        return;
    }
}

void handle_command(std::unique_ptr<gsdb::target>& target,
                    std::string_view line) {
    auto args = split(line, ' ');
    auto command = args[0];
    auto process = &target->get_process();

    if (is_prefix(command, "continue")) {
        /*if (std::string_view{"continue"}.starts_with(command)) {*/
        process->resume();
        auto reason = process->wait_on_signal();
        handle_stop(*target, reason);
    } else if (is_prefix(command, "register")) {
        handle_register_command(*process, args);
    } else if (is_prefix(command, "help")) {
        print_help(args);
    } else if (is_prefix(command, "breakpoint")) {
        handle_breakpoint_command(*process, args);
    } else if (is_prefix(command, "step")) {
        auto reason = process->step_instruction();
        handle_stop(*target, reason);
    } else if (is_prefix(command, "memory")) {
        handle_memory_command(*process, args);
    } else if (is_prefix(command, "disassemble")) {
        handle_disassemble_command(*process, args);
    } else if (is_prefix(command, "watchpoint")) {
        handle_watchpoint_command(*process, args);
    } else if (is_prefix(command, "catchpoint")) {
        handle_catchpoint_command(*process, args);
    } else {
        std::cerr << "Unknown command!\n";
    }
}

void main_loop(std::unique_ptr<gsdb::target>& target) {
    char* line = nullptr;

    while ((line = readline("gsdb> ")) != nullptr) {
        std::string line_str;

        if (line == std::string_view("")) {
            // if user hits Enter on empty line,
            // retrieve the previous command from history
            free(line);
            if (history_length > 0) {  // a global variable
                line_str = history_list()[history_length - 1]->line;
            }
        } else {
            line_str = line;
            add_history(line);  // searchable history
            free(line);
        }

        if (!line_str.empty()) {
            try {
                handle_command(target, line_str);
            } catch (const gsdb::error& err) {
                std::cout << err.what() << '\n';
            }
        }
    }
}

}  // namespace

int main(int argc, const char** argv) {
    if (argc == 1) {
        std::cerr << "No arguments given\n";
        return -1;
    }

    try {
        auto target = attach(argc, argv);
        g_gsdb_process = &target->get_process();
        signal(SIGINT, handle_sigint);
        main_loop(target);
    } catch (const gsdb::error& err) {
        std::cout << err.what() << '\n';
    }
}
