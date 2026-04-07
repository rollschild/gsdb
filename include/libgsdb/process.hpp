#ifndef GSDB_PROCESS_HPP
#define GSDB_PROCESS_HPP

#include <sys/types.h>
#include <sys/user.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

#include "libgsdb/register_info.hpp"
#include "libgsdb/registers.hpp"
#include "libgsdb/types.hpp"

namespace gsdb {

enum class process_state { stopped, running, exited, terminated };

struct stop_reason {
    stop_reason(int wait_status);

    process_state reason;
    std::uint8_t info;  // coming from `waitpid()`
};

class process {
   public:
    static std::unique_ptr<process> launch(
        std::filesystem::path path, bool debug = true,
        std::optional<int> stdout_replacement = std::nullopt);
    static std::unique_ptr<process> attach(pid_t pid);

    void resume();
    stop_reason wait_on_signal();
    pid_t pid() const { return pid_; }
    process_state state() const { return state_; }

    process() = delete;
    process(const process&) = delete;
    process& operator=(const process&) = delete;

    ~process();

    registers& get_registers() { return *registers_; }
    const registers& get_registers() const { return *registers_; }

    /**
     * Get program counter
     */
    virt_addr get_pc() const {
        return virt_addr{
            get_registers().read_by_id_as<std::uint64_t>(register_id::rip)};
    }

    void write_user_area(std::size_t offset, std::uint64_t data);

    void write_fprs(const user_fpregs_struct& fprs);
    void write_gprs(const user_regs_struct& gprs);

   private:
    // private constructor so that client code must use the static `launch` and
    // `attach` functions to construct the `process` object
    process(pid_t pid, bool terminate_on_end, bool is_attached)
        : pid_(pid),
          terminate_on_end_(terminate_on_end),
          is_attached_(is_attached),
          registers_(new registers(*this)) {}
    pid_t pid_ = 0;
    bool terminate_on_end_ = true;
    process_state state_ = process_state::stopped;
    bool is_attached_ = true;

    // populate registers_ when the process halts
    void read_all_registers();
    std::unique_ptr<registers> registers_;
};
}  // namespace gsdb

#endif
