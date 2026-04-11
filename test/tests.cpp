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
#include "libgsdb/types.hpp"

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

    regs.write_by_id(register_id::mm0, 0xdeadbeef);

    proc->resume();
    proc->wait_on_signal();

    output = channel.read();
    REQUIRE(to_string_view(output) == "0xdeadbeef");

    // test floating points
    regs.write_by_id(register_id::xmm0, 42.24);

    proc->resume();
    proc->wait_on_signal();

    output = channel.read();
    REQUIRE(to_string_view(output) == "42.24");

    regs.write_by_id(register_id::st0, 42.24l);
    // `fsw`: FPU status word, 16 bits wide
    //   - tracks current size of the FPU stack
    //   - reports errors like stack overflows, precision errors, or divisions
    //   by zero
    //   - bits 11 - 13 track top of the stack
    // `ftw`: FPU tag word
    //   - tracks which of the st registers are valid/empty/special (contain
    //   NaNs or infinity)
    //   - tag of `0b11` means empty
    //   - `0b00` means valid
    //
    // to push value to the stack, set bits 11 to 13 to 7 (0b111) - physical
    // register 7
    // The top starts at index 0 and goes down
    // A push decrements TOP. Starting from 0, one push wraps it to 7 (since 0 -
    // 1 mod 8 = 7)
    // So setting TOP=7 simulates the state _after_ one value has been pushed
    // onto a previously empty stack. The FPU sees "one push happened, st0 is at
    // R7, and it's valid"
    regs.write_by_id(register_id::fsw, std::uint16_t{0b0011100000000000});
    // set first tag to 0b00 and rest to 0b11
    regs.write_by_id(register_id::ftw, std::uint16_t{0b0011111111111111});

    proc->resume();
    proc->wait_on_signal();

    output = channel.read();
    REQUIRE(to_string_view(output) == "42.24");
}

TEST_CASE("Read register works", "[register]") {
    bool close_on_exec = false;
    gsdb::pipe channel(close_on_exec);

    auto proc = process::launch(std::string(TARGETS_DIR) + "/reg_read");
    auto& regs = proc->get_registers();

    proc->resume();  // resume again until next trap
    proc->wait_on_signal();

    REQUIRE(regs.read_by_id_as<std::uint64_t>(register_id::r13) == 0xcafecafe);

    proc->resume();
    proc->wait_on_signal();

    REQUIRE(regs.read_by_id_as<std::uint8_t>(register_id::r13b) == 42);

    proc->resume();
    proc->wait_on_signal();

    // NOTE the `ull` suffix to ensure the integer is `unsigned long long` -
    // `uint64_t`
    REQUIRE(regs.read_by_id_as<byte64>(register_id::mm0) ==
            to_byte64(0xdeadbeefull));

    proc->resume();
    proc->wait_on_signal();

    // Generally, you shouldn’t compare floating-point values by their bit
    // representations due to precision and rounding issues. However, binary can
    // exactly represent floating-point values whose denominators are powers of
    // two (such as 0.5, 0.25, 0.125, and so on), so it’s safe for us to do so
    // here.
    // We call such floating-point numbers **dyadic rationals**
    REQUIRE(regs.read_by_id_as<byte128>(register_id::xmm0) ==
            to_byte128(64.125));

    proc->resume();
    proc->wait_on_signal();

    // NOTE the `L` suffix - `long double`
    REQUIRE(regs.read_by_id_as<long double>(register_id::st0) == 64.125L);
}

TEST_CASE("Can create breakpoint site", "[breakpoint]") {
    auto proc = process::launch(std::string(TARGETS_DIR) + "/run_endlessly");
    auto& site = proc->create_breakpoint_site(virt_addr{42});
    REQUIRE(site.address().addr() == 42);
}

TEST_CASE("Breakpoint site IDs increase", "[breakpoint]") {
    auto proc = process::launch(std::string(TARGETS_DIR) + "/run_endlessly");

    auto& s1 = proc->create_breakpoint_site(virt_addr{42});
    REQUIRE(s1.address().addr() == 42);

    auto& s2 = proc->create_breakpoint_site(virt_addr{43});
    REQUIRE(s2.id() == s1.id() + 1);

    auto& s3 = proc->create_breakpoint_site(virt_addr{44});
    REQUIRE(s3.id() == s1.id() + 2);

    auto& s4 = proc->create_breakpoint_site(virt_addr{45});
    REQUIRE(s4.id() == s1.id() + 3);
}

TEST_CASE("Can find breakpoint site", "[breakpoint]") {
    auto proc = process::launch(std::string(TARGETS_DIR) + "/run_endlessly");
    const auto* cproc = proc.get();

    proc->create_breakpoint_site(virt_addr{42});
    proc->create_breakpoint_site(virt_addr{43});
    proc->create_breakpoint_site(virt_addr{44});
    proc->create_breakpoint_site(virt_addr{45});

    auto& s1 = proc->breakpoint_sites().get_by_address(virt_addr{44});
    REQUIRE(proc->breakpoint_sites().contains_address(virt_addr{44}));
    REQUIRE(s1.address().addr() == 44);

    auto& cs1 = cproc->breakpoint_sites().get_by_address(virt_addr{44});
    REQUIRE(cproc->breakpoint_sites().contains_address(virt_addr{44}));
    REQUIRE(cs1.address().addr() == 44);

    auto& s2 = proc->breakpoint_sites().get_by_id(s1.id() + 1);
    REQUIRE(proc->breakpoint_sites().contains_id(s1.id() + 1));
    REQUIRE(s2.address().addr() == 45);

    auto& cs2 = cproc->breakpoint_sites().get_by_id(cs1.id() + 1);
    REQUIRE(cproc->breakpoint_sites().contains_id(cs1.id() + 1));
    REQUIRE(cs2.address().addr() == 45);
    REQUIRE(cs2.id() == cs1.id() + 1);
}

TEST_CASE("Cannot find breakpoint site", "[breakpoint]") {
    auto proc = process::launch(std::string(TARGETS_DIR) + "/run_endlessly");
    const auto* cproc = proc.get();

    REQUIRE_THROWS_AS(proc->breakpoint_sites().get_by_address(virt_addr{44}),
                      error);
    REQUIRE_THROWS_AS(proc->breakpoint_sites().get_by_id(44), error);
    REQUIRE_THROWS_AS(cproc->breakpoint_sites().get_by_address(virt_addr{44}),
                      error);
    REQUIRE_THROWS_AS(cproc->breakpoint_sites().get_by_id(44), error);
}

TEST_CASE("Breakpoint site list size and emptiness", "[breakpoint]") {
    auto proc = process::launch(std::string(TARGETS_DIR) + "/run_endlessly");
    const auto* cproc = proc.get();

    REQUIRE(proc->breakpoint_sites().empty());
    REQUIRE(proc->breakpoint_sites().size() == 0);
    REQUIRE(cproc->breakpoint_sites().empty());
    REQUIRE(cproc->breakpoint_sites().size() == 0);

    proc->create_breakpoint_site(virt_addr{42});
    REQUIRE(!proc->breakpoint_sites().empty());
    REQUIRE(proc->breakpoint_sites().size() == 1);
    REQUIRE(!cproc->breakpoint_sites().empty());
    REQUIRE(cproc->breakpoint_sites().size() == 1);

    proc->create_breakpoint_site(virt_addr{43});
    REQUIRE(!proc->breakpoint_sites().empty());
    REQUIRE(proc->breakpoint_sites().size() == 2);
    REQUIRE(!cproc->breakpoint_sites().empty());
    REQUIRE(cproc->breakpoint_sites().size() == 2);
}

TEST_CASE("Can iterate breakpoint sites", "[breakpoint]") {
    auto proc = process::launch(std::string(TARGETS_DIR) + "/run_endlessly");
    const auto* cproc = proc.get();

    proc->create_breakpoint_site(virt_addr{42});
    proc->create_breakpoint_site(virt_addr{43});
    proc->create_breakpoint_site(virt_addr{44});
    proc->create_breakpoint_site(virt_addr{45});

    proc->breakpoint_sites().for_each(
        // init-captures
        // creates an capture with initial value 42
        // `mutable` enables us to change the value of this capture during
        // iteration; otherwise the capture would be declared `const`
        [addr = uint64_t{42}](auto& site) mutable {
            REQUIRE(site.address().addr() == addr++);
        });
    cproc->breakpoint_sites().for_each(
        // init-captures
        // creates an capture with initial value 42
        // `mutable` enables us to change the value of this capture during
        // iteration; otherwise the capture would be declared `const`
        [addr = uint64_t{42}](auto& site) mutable {
            REQUIRE(site.address().addr() == addr++);
        });
}
