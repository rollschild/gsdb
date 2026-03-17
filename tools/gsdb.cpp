#include <editline/readline.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "libgsdb/error.hpp"
#include "libgsdb/process.hpp"

namespace {
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
        const char* program_path = argv[1];
        return gsdb::process::launch(program_path);
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

[[maybe_unused]]
bool is_prefix(std::string_view str, std::string_view of) {
    if (str.size() > of.size()) return false;
    return std::equal(str.begin(), str.end(), of.begin());
}

void print_stop_reason(const gsdb::process& process, gsdb::stop_reason reason) {
    std::cout << "Process " << process.pid() << ' ';

    switch (reason.reason) {
        case gsdb::process_state::exited:
            std::cout << "exited with status " << static_cast<int>(reason.info);
            break;
        case gsdb::process_state::terminated:
            std::cout << "terminated with signal " << sigabbrev_np(reason.info);
            break;
        case gsdb::process_state::stopped:
            std::cout << "stopped with signal " << sigabbrev_np(reason.info);
            break;
        default:
            std::cout << "still running!";
            break;
    }

    std::cout << std::endl;
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
