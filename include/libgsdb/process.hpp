#ifndef GSDB_PROCESS_HPP
#define GSDB_PROCESS_HPP

#include <sys/types.h>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace gsdb {

enum class process_state { stopped, running, exited, terminated };

struct stop_reason {
    stop_reason(int wait_status);

    process_state reason;
    std::uint8_t info;  // coming from `waitpid()`
};

class process {
   public:
    static std::unique_ptr<process> launch(std::filesystem::path path,
                                           bool debug = true);
    static std::unique_ptr<process> attach(pid_t pid);

    void resume();
    stop_reason wait_on_signal();
    pid_t pid() const { return pid_; }
    process_state state() const { return state_; }

    process() = delete;
    process(const process&) = delete;
    process& operator=(const process&) = delete;

    ~process();

   private:
    // private constructor so that client code must use the static `launch` and
    // `attach` functions to construct the `process` object
    process(pid_t pid, bool terminate_on_end, bool is_attached)
        : pid_(pid),
          terminate_on_end_(terminate_on_end),
          is_attached_(is_attached) {}
    pid_t pid_ = 0;
    bool terminate_on_end_ = true;
    process_state state_ = process_state::stopped;
    bool is_attached_ = true;
};
}  // namespace gsdb

#endif
