#include <editline/readline.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <concepts>
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
#include "libgsdb/error.hpp"
#include "libgsdb/parse.hpp"
#include "libgsdb/process.hpp"
#include "libgsdb/register_info.hpp"
#include "libgsdb/registers.hpp"
#include "libgsdb/types.hpp"

namespace {
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
register    - Commands for operating on registers
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
)";
    } else {
        std::cerr << "No help available on that\n";
    }
}
/**
 * Launches, attaches to the given program name or PID.
 * Returns the PID of the inferior.
 */
std::unique_ptr<gsdb::process> attach(int argc, const char** argv) {
    // passing PID
    if (argc == 3 && argv[1] == std::string_view("-p")) {
        // std::string() would have dynamically allocated memory
        pid_t pid = std::atoi(argv[2]);
        return gsdb::process::attach(pid);
    } else {
        // passing program name
        auto program_path = argv[1];
        auto proc = gsdb::process::launch(program_path);
        std::print("Launched process with PID {}\n", proc->pid());
        return proc;
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

void print_stop_reason(const gsdb::process& process, gsdb::stop_reason reason) {
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
            message =
                std::format("stopped with signal {} at {:#x}",
                            sigabbrev_np(reason.info), process.get_pc().addr());
            break;
        default:
            message = "still running!";
            break;
    }

    std::print("Process {} {}\n", process.pid(), message);
}

template <std::ranges::range T>
// requires std::formattable<std::ranges::range_value_t<T>, char>
std::string format_join(const T& t, std::string_view separator) {
    std::string res;
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
    return res;
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

        process.create_breakpoint_site(gsdb::virt_addr{*address}).enable();
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

void handle_command(std::unique_ptr<gsdb::process>& process,
                    std::string_view line) {
    auto args = split(line, ' ');
    auto command = args[0];

    if (is_prefix(command, "continue")) {
        /*if (std::string_view{"continue"}.starts_with(command)) {*/
        process->resume();
        auto reason = process->wait_on_signal();
        print_stop_reason(*process, reason);
    } else if (is_prefix(command, "register")) {
        handle_register_command(*process, args);
    } else if (is_prefix(command, "help")) {
        print_help(args);
    } else if (is_prefix(command, "breakpoint")) {
        handle_breakpoint_command(*process, args);
    } else {
        std::cerr << "Unknown command!\n";
    }
}

void main_loop(std::unique_ptr<gsdb::process>& process) {
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
                handle_command(process, line_str);
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
        auto process = attach(argc, argv);
        main_loop(process);
    } catch (const gsdb::error& err) {
        std::cout << err.what() << '\n';
    }
}
