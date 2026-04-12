#include <sys/personality.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <libgsdb/error.hpp>
#include <libgsdb/process.hpp>
#include <memory>
#include <optional>
#include <string>

#include "libgsdb/breakpoint_site.hpp"
#include "libgsdb/pipe.hpp"
#include "libgsdb/register_info.hpp"

namespace {
void exit_with_perror(gsdb::pipe& channel, std::string const& prefix) {
    auto message = prefix + ": " + std::strerror(errno);
    channel.write(reinterpret_cast<std::byte*>(message.data()), message.size());
    exit(-1);
}
}  // namespace

gsdb::process::~process() {
    if (pid_ != 0) {
        int status;
        if (is_attached_) {
            if (state_ == process_state::running) {
                kill(pid_, SIGSTOP);
                waitpid(pid_, &status, 0);
            }

            // for PTRACE_DETACH to work, the inferior must be stopped
            // thus the SIGSTOP above
            ptrace(PTRACE_DETACH, pid_, nullptr, nullptr);
            kill(pid_, SIGCONT);  // detached, then let it continue
        }
        if (terminate_on_end_) {
            kill(pid_, SIGKILL);
            waitpid(pid_, &status, 0);  // wait for it to terminate
        }
    }
}

/**
 * Force the process to resume and update its tracked running state
 */
void gsdb::process::resume() {
    auto pc = get_pc();
    // First, check if we are at breakpoint
    if (breakpoint_sites_.enabled_stoppoint_at_address(pc)) {
        auto& bp = breakpoint_sites_.get_by_address(pc);
        // if at breakpoint, disable it
        bp.disable();
        if (ptrace(PTRACE_SINGLESTEP, pid_, nullptr, nullptr) < 0) {
            // execute a single instruction
            error::send_errno("Failed to single step!");
        }
        int wait_status;
        // wait until the inferior has executed the instruction and halted
        if (waitpid(pid_, &wait_status, 0) < 0) {
            error::send_errno("waitpid failed!");
        }
        // re-enable the breakpoint - patch 0xcc back in, before continuing the
        // process and setting the state to running
        bp.enable();
    }
    if (ptrace(PTRACE_CONT, pid_, nullptr, nullptr) < 0) {
        error::send_errno("Could NOT resume");
    }
    state_ = process_state::running;
}

gsdb::stop_reason::stop_reason(int wait_status) {
    if (WIFEXITED(wait_status)) {
        reason = process_state::exited;
        info = WEXITSTATUS(wait_status);
    } else if (WIFSIGNALED(wait_status)) {
        reason = process_state::terminated;
        info = WTERMSIG(wait_status);
    } else if (WIFSTOPPED(wait_status)) {
        reason = process_state::stopped;
        info = WSTOPSIG(wait_status);
    }
}

gsdb::stop_reason gsdb::process::wait_on_signal() {
    int wait_status;
    int options = 0;
    if (waitpid(pid_, &wait_status, options) < 0) {
        error::send_errno("waitpid FAILED");
    }
    stop_reason reason(wait_status);
    state_ = reason.reason;

    // read all registers so long as we are attached to this process and
    // it is stopped
    if (is_attached_ and state_ == process_state::stopped) {
        read_all_registers();

        // If process stopped due to `SIGTRAP` and the address 1 byte below the
        // program counter is an enabled breakpoint, we should fix up the
        // program counter to point to the breakpoint
        auto instr_begin = get_pc() - 1;
        if (reason.info == SIGTRAP and
            breakpoint_sites_.enabled_stoppoint_at_address(instr_begin)) {
            set_pc(instr_begin);
        }
    }

    return reason;
}

std::unique_ptr<gsdb::process> gsdb::process::launch(
    std::filesystem::path path, bool debug,
    std::optional<int> stdout_replacement) {
    // IMPORTANT: CALL pipe BEFORE fork
    pipe channel(/*close_on_exec=*/true);

    pid_t pid;
    if ((pid = fork()) < 0) {
        error::send_errno("fork FAILED");
    }

    // 1. PTRACE_TRACEME — "I want to be traced" (no stop)
    // 2. execlp — kernel replaces the process image, then delivers SIGTRAP →
    // child stops
    // 3. Parent's waitpid returns, confirming the child is stopped and ready to
    // be debugged
    if (pid == 0) {
        // child process
        personality(ADDR_NO_RANDOMIZE);  // NO ASLR
        channel.close_read();

        if (stdout_replacement) {
            // dup2: closes the second file descriptor, and then duplicates the
            // first file descriptor to the second.
            // Now anything that goes to `stdout` now goes through
            // `*stdout_replacement`.
            // Could also use `stdout_replacement.value()`
            if (dup2(*stdout_replacement, STDOUT_FILENO) < 0) {
                exit_with_perror(channel, "stdout replacement failed");
            }
        }

        if (debug and ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) < 0) {
            exit_with_perror(channel, "Tracing failed");
        }
        if (execlp(path.c_str(), path.c_str(), nullptr) < 0) {
            // child stops here
            // kernel automatically sends it a `SIGTRAP`, which stops it before
            // the new program executes any instructions.
            // This is what the parent's `wait_on_signal` is waiting for
            exit_with_perror(channel, "exec FAILED");
        }
    }

    channel.close_write();
    auto data = channel.read();
    channel.close_read();
    if (data.size() > 0) {
        // wait for child process to terminate
        waitpid(pid, nullptr, 0);
        // Now whenever there is an error in launching the child process, the
        // parent process will throw an exception
        auto chars = reinterpret_cast<char*>(data.data());
        //               ↓begin-pointer↓  ↓end-pointer↓
        error::send(std::string(chars, chars + data.size()));
    }

    std::unique_ptr<process> proc(
        new process(pid, /*terminate_on_end=*/true, debug));
    // if we don't start tracing we should _not_ wait for a signal after
    // forking, because the process won't stop automatically after the `exec`
    // call
    if (debug) {
        proc->wait_on_signal();  // wait for the process to halt
    }

    return proc;
}

std::unique_ptr<gsdb::process> gsdb::process::attach(pid_t pid) {
    if (pid == 0) {
        error::send("INVALID PID");
    }

    if (ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) < 0) {
        // sends a SIGSTOP to the target process
        error::send_errno("Could NOT attach");
    }

    std::unique_ptr<process> proc(
        new process(pid, /*terminate_on_end=*/false, /*is_attached=*/true));
    proc->wait_on_signal();  // blocking, wait for the underlying process to
                             // halt

    return proc;
}

void gsdb::process::read_all_registers() {
    if (ptrace(PTRACE_GETREGS, pid_, nullptr, &get_registers().data_.regs) <
        0) {
        error::send_errno("Could not read GPR registers");
    }
    if (ptrace(PTRACE_GETFPREGS, pid_, nullptr, &get_registers().data_.i387) <
        0) {
        error::send_errno("Could not read FPR registers");
    }
    // essentially loop over scoped enum values
    for (int i = 0; i < 8; ++i) {
        auto id = static_cast<int>(register_id::dr0) + i;
        auto info = register_info_by_id(static_cast<register_id>(id));

        errno = 0;

        std::uint64_t data =
            ptrace(PTRACE_PEEKUSER, pid_, info.offset, nullptr);
        if (errno != 0) {
            error::send_errno(
                std::format("Could not read debug register {:d}", i));
        }
        get_registers().data_.u_debugreg[i] = data;
    }
}

/**
 * write the given data to the user area at the given offset.
 */
void gsdb::process::write_user_area(std::size_t offset, std::uint64_t data) {
    if (ptrace(PTRACE_POKEUSER, pid_, offset, data) < 0) {
        error::send_errno("Could not write to user area");
    }
}

void gsdb::process::write_fprs(const user_fpregs_struct& fprs) {
    if (ptrace(PTRACE_SETFPREGS, pid_, nullptr, &fprs) < 0) {
        error::send_errno("Could not write floating point registers");
    }
}
void gsdb::process::write_gprs(const user_regs_struct& gprs) {
    if (ptrace(PTRACE_SETREGS, pid_, nullptr, &gprs) < 0) {
        error::send_errno("Could not write general purpose registers");
    }
}

gsdb::breakpoint_site& gsdb::process::create_breakpoint_site(
    virt_addr address) {
    if (breakpoint_sites_.contains_address(address)) {
        error::send("Breakpoint site already created at address " +
                    std::to_string(address.addr()));
    }
    return breakpoint_sites_.push(
        std::unique_ptr<breakpoint_site>(new breakpoint_site(*this, address)));
}
