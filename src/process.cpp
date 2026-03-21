#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <libgsdb/error.hpp>
#include <libgsdb/process.hpp>
#include <memory>
#include <string>

#include "libgsdb/pipe.hpp"

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
        if (state_ == process_state::running) {
            kill(pid_, SIGSTOP);
            waitpid(pid_, &status, 0);
        }

        // for PTRACE_DETACH to work, the inferior must be stopped
        // thus the SIGSTOP above
        ptrace(PTRACE_DETACH, pid_, nullptr, nullptr);
        kill(pid_, SIGCONT);  // detached, then let it continue

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
    return reason;
}

std::unique_ptr<gsdb::process> gsdb::process::launch(
    std::filesystem::path path) {
    // IMPORTANT: CALL pipe BEFORE fork
    pipe channel(/*close_on_exec=*/true);

    pid_t pid;
    if ((pid = fork()) < 0) {
        error::send_errno("fork FAILED");
    }

    if (pid == 0) {
        // child process
        channel.close_read();
        if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) < 0) {
            error::send_errno("Tracing failed");
            exit_with_perror(channel, "Tracing failed");
        }
        if (execlp(path.c_str(), path.c_str(), nullptr) < 0) {
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

    std::unique_ptr<process> proc(new process(pid, /*terminate_on_end=*/true));
    proc->wait_on_signal();  // wait for the process to halt

    return proc;
}

std::unique_ptr<gsdb::process> gsdb::process::attach(pid_t pid) {
    if (pid == 0) {
        error::send("INVALID PID");
    }

    if (ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) < 0) {
        error::send_errno("Could NOT attach");
    }

    std::unique_ptr<process> proc(new process(pid, /*terminate_on_end=*/false));
    proc->wait_on_signal();  // wait for the underlying process to halt

    return proc;
}
