#include <elf.h>
#include <sys/personality.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <libgsdb/error.hpp>
#include <libgsdb/process.hpp>
#include <libgsdb/target.hpp>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "libgsdb/bit.hpp"
#include "libgsdb/breakpoint_site.hpp"
#include "libgsdb/pipe.hpp"
#include "libgsdb/register_info.hpp"
#include "libgsdb/types.hpp"
#include "libgsdb/watchpoint.hpp"

namespace {
void exit_with_perror(gsdb::pipe& channel, std::string const& prefix) {
    auto message = prefix + ": " + std::strerror(errno);
    channel.write(reinterpret_cast<std::byte*>(message.data()), message.size());
    exit(-1);
}

/**
 * Signal number will have its 8th bit set if the SIGTRAP came from a syscall.
 * We can use `signal == (SIGTRAP | 0x80)` to check whether we're trapped by a
 * syscall.
 */
void set_ptrace_options(pid_t pid) {
    if (ptrace(PTRACE_SETOPTIONS, pid, nullptr, PTRACE_O_TRACESYSGOOD) < 0) {
        gsdb::error::send_errno("Failed to set TRACESYSGOOD option!");
    }
}

/*
00b Instruction execution only
01b Data writes only
10b I/O reads and writes (generally unsupported)
11b Data reads and writes
*/
std::uint64_t encode_hardware_stoppoint_mode(gsdb::stoppoint_mode mode) {
    switch (mode) {
        case gsdb::stoppoint_mode::write:
            return 0b01;
        case gsdb::stoppoint_mode::read_write:
            return 0b11;
        case gsdb::stoppoint_mode::execute:
            return 0b00;
        default:
            gsdb::error::send("Invalid stoppoint mode!");
    }
}

/*
00b 1 byte
01b 2 bytes
10b 8 bytes
11b 4 bytes
*/
std::uint64_t encode_hardware_stoppoint_size(std::size_t size) {
    switch (size) {
        case 1:
            return 0b00;
        case 2:
            return 0b01;
        case 4:
            return 0b11;
        case 8:
            return 0b10;
        default:
            gsdb::error::send("Invalid stoppoint size!");
    }
}

/*
 * To find the first free DR register, we’ll check the two enable bits in the
 * control register that correspond to each DR register until we find one that
 * has no bits set
 */
int find_free_stoppoint_register(std::uint64_t control_register) {
    for (auto i = 0; i < 4; ++i) {
        if ((control_register & (0b11 << (i * 2))) == 0) {
            return i;
        }
    }
    gsdb::error::send("No remaining hardware debug registers!");
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
    auto request =
        syscall_catch_policy_.get_mode() == syscall_catch_policy::mode::none
            ? PTRACE_CONT
            : PTRACE_SYSCALL;  // Now the inferior will trap whenever a syscall
                               // is entered or exited
    if (ptrace(request, pid_, nullptr, nullptr) < 0) {
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
        augment_stop_reason(reason);

        // If process stopped due to `SIGTRAP` and the address 1 byte below the
        // program counter is an enabled breakpoint, we should fix up the
        // program counter to point to the breakpoint
        auto instr_begin = get_pc() - 1;
        if (reason.info == SIGTRAP) {
            if (reason.trap_reason == trap_type::software_break and
                breakpoint_sites_.enabled_stoppoint_at_address(instr_begin) and
                breakpoint_sites_.get_by_address(instr_begin).is_enabled()) {
                // if caused by software breakpoint, walk the program counter
                // back 1 byte to the start of the `int3` instruction
                set_pc(instr_begin);
            } else if (reason.trap_reason == trap_type::hardware_break) {
                // if caused by hardware stoppoint
                auto id = get_current_hardware_stoppoint();
                // id is a variant -> check if it's watchpoint
                if (id.index() == 1) {
                    watchpoints_.get_by_id(std::get<1>(id)).update_data();
                }
            } else if (reason.trap_reason == trap_type::syscall) {
                reason = maybe_resume_from_syscall(reason);
            }
        }
        if (target_) {
            target_->notify_stop(reason);
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

        // set PGID of child to be same as its PID
        // instead of using parant's PID as PGID
        if (setpgid(0, 0) < 0) {
            exit_with_perror(channel, "Could not set pgid!");
        }
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
        // wait for the process to halt
        proc->wait_on_signal();
        // extend the signal information that `ptrace` provides, so it becomes
        // much easier to distinguish `SIGTRAP` signals that come from syscalls
        set_ptrace_options(proc->pid());
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
    set_ptrace_options(proc->pid());

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

gsdb::breakpoint_site& gsdb::process::create_breakpoint_site(virt_addr address,
                                                             bool hardware,
                                                             bool internal) {
    if (breakpoint_sites_.contains_address(address)) {
        error::send("Breakpoint site already created at address " +
                    std::to_string(address.addr()));
    }
    return breakpoint_sites_.push(std::unique_ptr<breakpoint_site>(
        new breakpoint_site(*this, address, hardware, internal)));
}

gsdb::stop_reason gsdb::process::step_instruction() {
    // track the breakpoint site at which the process is currently stopped
    std::optional<breakpoint_site*> to_reenable;
    auto pc = get_pc();
    if (breakpoint_sites_.enabled_stoppoint_at_address(pc)) {
        // we must declare bp as a reference, so that we can safely store a
        // pointer to it in a variable declared in the enclosing scope
        auto& bp = breakpoint_sites_.get_by_address(pc);
        bp.disable();
        to_reenable = &bp;
    }

    if (ptrace(PTRACE_SINGLESTEP, pid_, nullptr, nullptr) < 0) {
        error::send_errno("Could not single step!");
    }

    // wait until the single step over is completed
    auto reason = wait_on_signal();
    if (to_reenable) {
        to_reenable.value()->enable();
    }

    return reason;
}

/**
 * Read `amount` bytes from the address space of a traced process into the
 * debugger's own memory
 */
std::vector<std::byte> gsdb::process::read_memory(virt_addr address,
                                                  std::size_t amount) const {
    std::vector<std::byte> ret(amount);

    /* iovec (I/O vector) is a POSIX struct defined in <sys/uio.h>:

    struct iovec {
      void  *iov_base;  // starting address of buffer
      size_t iov_len;   // size of buffer
    };

    It describes a contiguous region of memory — a base pointer
    and a length. It's the standard building block for
    scatter/gather I/O operations (like readv, writev, and here
    process_vm_readv). */
    // description of memeory involved in the data transfer
    // destination of memory copy
    iovec local_desc{ret.data(), ret.size()};

    // The kernel _gathers_ from the remote iovecs and _scatters_ into
    // the local iovec (in practice it fills the local buffer
    // sequentially). Returns the number of bytes read, or -1 on
    // error.
    std::vector<iovec> remote_descs;

    while (amount > 0) {
        // split the range of data to be copied on memory page boundaries
        // assuming 4KiB (4096, 0x1000) pages - default on x64
        // 0xfff - 12-bit mask, matching the 12-bit page offset in a 4096-byte
        // page (2 ^ 12 = 4096)
        auto up_to_next_page = 0x1000 - (address.addr() & 0xfff);
        auto chunk_size = std::min(amount, up_to_next_page);
        remote_descs.push_back(
            {reinterpret_cast<void*>(address.addr()), chunk_size});
        amount -= chunk_size;
        address += chunk_size;
    }

    if (process_vm_readv(pid_, &local_desc, /*liovcnt=*/1, remote_descs.data(),
                         /*riovcnt=*/remote_descs.size(), /*flags=*/0) < 0) {
        error::send_errno("Could not read process memory");
    }

    return ret;
}

void gsdb::process::write_memory(virt_addr address,
                                 span<const std::byte> data) {
    std::size_t written = 0;
    while (written < data.size()) {
        auto remaining = data.size() - written;
        std::uint64_t word;
        if (remaining >= 8) {
            word = from_bytes<std::uint64_t>(data.begin() + written);
        } else {
            auto read = read_memory(address + written, 8);
            // cast to char* so that we can use `memcpy` on it
            auto word_data = reinterpret_cast<char*>(&word);
            std::memcpy(word_data, data.begin() + written, remaining);
            // restore the original data
            // make sure not to overwrite the `8 - remaining` part of the data
            std::memcpy(word_data + remaining, read.data() + remaining,
                        8 - remaining);
        }

        if (ptrace(PTRACE_POKEDATA, pid_, address + written, word) < 0) {
            error::send_errno("Failed to write memory!");
        }

        written += 8;
    }
}

std::vector<std::byte> gsdb::process::read_memory_without_traps(
    virt_addr address, std::size_t amount) const {
    auto memory = read_memory(address, amount);
    auto sites = breakpoint_sites_.get_in_region(address, address + amount);

    for (auto site : sites) {
        // should ignore hardware breakpoints
        if (!site->is_enabled() or site->is_hardware()) continue;
        auto offset = site->address() - address.addr();
        memory[offset.addr()] = site->saved_data_;
    }

    return memory;
}

int gsdb::process::set_hardware_breakpoint(
    [[maybe_unused]] gsdb::breakpoint_site::id_type id, virt_addr address) {
    // size for execution-only hardware breakpoints must be 1
    return set_hardware_stoppoint(address, stoppoint_mode::execute, 1);
}

int gsdb::process::set_hardware_stoppoint(gsdb::virt_addr address,
                                          stoppoint_mode mode,
                                          std::size_t size) {
    auto& regs = get_registers();
    // read the control register (DR7) and find a free spot
    auto control = regs.read_by_id_as<std::uint64_t>(register_id::dr7);

    // returns 0, 1, 2, or 3, depending on which register is free
    // or throw exception if there is no free space
    // for DR0..3
    int free_space = find_free_stoppoint_register(control);
    // write the given address to the DR register corresponding to the free
    // space found
    auto id = static_cast<int>(register_id::dr0) + free_space;
    regs.write_by_id(static_cast<register_id>(id), address.addr());

    // encode the bits for mode and size
    auto mode_flag = encode_hardware_stoppoint_mode(mode);
    auto size_flag = encode_hardware_stoppoint_size(size);

    auto enable_bit = (1 << (free_space * 2));
    auto mode_bits = (mode_flag << (free_space * 4 + 16));
    auto size_bits = (size_flag << (free_space * 4 + 18));

    auto clear_mask =
        (0b11 << (free_space * 2)) | (0b1111 << (free_space * 4 + 16));
    auto masked = control & ~clear_mask;

    masked |= enable_bit | mode_bits | size_bits;

    regs.write_by_id(register_id::dr7, masked);

    return free_space;
}

void gsdb::process::clear_hardware_stoppoint(int index) {
    // write 0 to the DR register at the given index
    auto id = static_cast<int>(register_id::dr0) + index;
    get_registers().write_by_id(static_cast<register_id>(id), 0);

    auto control =
        get_registers().read_by_id_as<std::uint64_t>(register_id::dr7);

    auto clear_mask = (0b11 << (index * 2)) | (0b1111 << (index * 4 + 16));
    auto masked = control & ~clear_mask;

    get_registers().write_by_id(register_id::dr7, masked);
}

int gsdb::process::set_watchpoint([[maybe_unused]] gsdb::watchpoint::id_type id,
                                  virt_addr address, stoppoint_mode mode,
                                  std::size_t size) {
    return set_hardware_stoppoint(address, mode, size);
}

gsdb::watchpoint& gsdb::process::create_watchpoint(virt_addr address,
                                                   stoppoint_mode mode,
                                                   std::size_t size) {
    if (watchpoints_.contains_address(address)) {
        error::send("Watchpoint already created at address " +
                    std::to_string(address.addr()));
    }
    return watchpoints_.push(std::unique_ptr<watchpoint>(
        new watchpoint(*this, address, mode, size)));
}

void gsdb::process::augment_stop_reason(gsdb::stop_reason& reason) {
    siginfo_t info;
    if (ptrace(PTRACE_GETSIGINFO, pid_, nullptr, &info) < 0) {
        error::send_errno("Failed to get signal info!");
    }

    if (reason.info == (SIGTRAP | 0x80)) {
        // Default constructs a new syscall_information _in-place_
        // inside the optional's internal storage.
        // Mark the optional as engaged (has_value() == true).
        // Returns a non-const ref (T&) to the newly-built object that now lives
        // inside the optional
        auto& sys_info = reason.syscall_info.emplace();
        auto& regs = get_registers();

        if (expecting_syscall_exit_) {
            sys_info.entry = false;
            sys_info.id =
                regs.read_by_id_as<std::uint64_t>(register_id::orig_rax);
            sys_info.ret = regs.read_by_id_as<std::uint64_t>(register_id::rax);
            expecting_syscall_exit_ = false;
        } else {
            sys_info.entry = true;
            sys_info.id =
                regs.read_by_id_as<std::uint64_t>(register_id::orig_rax);
            std::array<register_id, 6> arg_regs = {
                register_id::rdi, register_id::rsi, register_id::rdx,
                register_id::r10, register_id::r8,  register_id::r9,
            };

            for (auto i = 0; i < 6; ++i) {
                sys_info.args[i] =
                    regs.read_by_id_as<std::uint64_t>(arg_regs[i]);
            }

            expecting_syscall_exit_ = true;
        }

        reason.info = SIGTRAP;
        reason.trap_reason = trap_type::syscall;
        return;
    }

    expecting_syscall_exit_ = false;

    reason.trap_reason = trap_type::unknown;
    if (reason.info == SIGTRAP) {
        switch (info.si_code) {
            case TRAP_TRACE:
                reason.trap_reason = trap_type::single_step;
                break;
            // x64 uses SI_KERNEL, not TRAP_BRKPT, for software breakpoints
            case SI_KERNEL:
                reason.trap_reason = trap_type::software_break;
                break;
            case TRAP_HWBKPT:
                reason.trap_reason = trap_type::hardware_break;
                break;
        }
    }
}

std::variant<gsdb::breakpoint_site::id_type, gsdb::watchpoint::id_type>
gsdb::process::get_current_hardware_stoppoint() const {
    auto& regs = get_registers();
    auto status = regs.read_by_id_as<std::uint64_t>(gsdb::register_id::dr6);
    // find which bit of the least significant 4 bits is set by couting trailing
    // zeros
    // (unsigned long long) flavor of the `__builtin_ctz` function
    auto index = __builtin_ctzll(status);

    auto id = static_cast<int>(gsdb::register_id::dr0) + index;
    auto addr = gsdb::virt_addr(
        regs.read_by_id_as<std::uint64_t>(static_cast<gsdb::register_id>(id)));

    using ret =
        std::variant<gsdb::breakpoint_site::id_type, gsdb::watchpoint::id_type>;
    if (breakpoint_sites_.contains_address(addr)) {
        auto site_id = breakpoint_sites_.get_by_address(addr).id();
        // setting the type at index 0 of the std::variant, which is
        // sdb::breakpoint _site::id_type
        return ret{std::in_place_index<0>, site_id};
    } else {
        auto watch_id = watchpoints_.get_by_address(addr).id();
        return ret{std::in_place_index<1>, watch_id};
    }
}

/**
 * Resume if the current syscall is the one we are catching
 */
gsdb::stop_reason gsdb::process::maybe_resume_from_syscall(
    const stop_reason& reason) {
    if (syscall_catch_policy_.get_mode() == syscall_catch_policy::mode::some) {
        auto& to_catch = syscall_catch_policy_.get_to_catch();
        auto found = std::find(std::begin(to_catch), std::end(to_catch),
                               reason.syscall_info->id);
        if (found == std::end(to_catch)) {
            resume();
            return wait_on_signal();
        }
    }

    return reason;
}

std::unordered_map<int, std::uint64_t> gsdb::process::get_auxv() const {
    auto path = "/proc/" + std::to_string(pid_) + "/auxv";
    std::ifstream auxv(path);

    std::unordered_map<int, std::uint64_t> ret;
    std::uint64_t id, value;

    auto read = [&](auto& into) {
        auxv.read(reinterpret_cast<char*>(&into), sizeof(into));
    };

    for (read(id); id != AT_NULL; read(id)) {
        read(value);
        ret[id] = value;
    }

    return ret;
}
