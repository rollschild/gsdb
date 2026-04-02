#include <sys/types.h>

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <csignal>
#include <fstream>
#include <libgsdb/bit.hpp>
#include <libgsdb/process.hpp>
#include <string>

#include "libgsdb/error.hpp"
#include "libgsdb/pipe.hpp"
#include "libgsdb/register_info.hpp"

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
}
