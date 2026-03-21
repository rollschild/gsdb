#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <csignal>
#include <libgsdb/process.hpp>

#include "libgsdb/error.hpp"

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
