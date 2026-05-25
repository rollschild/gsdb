# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Note to Claude

Do _NOT_ directly change source files, ever, except for `CLAUDE.md`. For `CLAUDE.md` you are free to make any changes on your own. Save any plans/proposals under `.claude/` directory at the project's root directory. Do _NOT_ touch my `$HOME` directory, ever.

## Project

gsdb is a Linux debugger written from scratch in C++23, built with CMake. It uses a Nix flake (`flake.nix`) for development environment setup. The debugger uses `ptrace` for process control and targets x86-64 ELF binaries with DWARF debug info. The project enables `C`, `CXX`, and `ASM` languages in CMake (assembly is used for test target binaries like `reg_write.s`).

## Environment Setup

```bash
# Enter the Nix dev shell (provides GCC, CMake, Catch2, libedit, etc.)
nix develop
```

The flake uses GCC (not Clang). The dev shell automatically switches to zsh with starship prompt.

## Build Commands

```bash
# Configure (out-of-source build required; in-source builds are blocked)
# NOTE: `nix develop` shell hook auto-runs this and symlinks compile_commands.json
cmake -S . -B build

# Build everything
cmake --build build

# Build a specific target
cmake --build build --target gsdb      # CLI tool
cmake --build build --target libgsdb   # library
cmake --build build --target tests     # test binary

# Run all tests
cd build && ctest

# Run the test binary directly
./build/test/tests

# Run a single Catch2 test case by name
./build/test/tests "test case name"

# Run tests by tag
./build/test/tests "[process]"
```

`compile_commands.json` is auto-generated and symlinked by CMakeLists.txt (no manual step needed).

## Architecture

```
 include/libgsdb/          (public headers)
 src/                      (library implementation)
        ↓
    [libgsdb.a]  (CMake target: gsdb::libgsdb, links Zydis::Zydis)
        ↓                          ↓
 + PkgConfig::libedit       + Catch2::Catch2WithMain
        ↓                          ↓
 tools/gsdb                 test/tests
 (CLI binary)               (test binary)
```

- **`src/`** → `libgsdb.a` (static library, CMake target `gsdb::libgsdb`). Public headers in `include/libgsdb/`, private headers in `src/include/` (currently holds the X-macro syscall table `syscalls.inc`). OUTPUT_NAME override prevents `liblibgsdb.a`.
- **`tools/`** → `gsdb` CLI. REPL via libedit, command parsing, attach logic (fork+exec with `PTRACE_TRACEME` or `PTRACE_ATTACH` to PID via `-p`).
- **`test/`** → Catch2 v3 tests (`Catch2::Catch2WithMain` supplies `main()`). Debuggee target binaries live in `test/targets/` (C++ and assembly, assembly uses `-pie`). Tests reference them via the `TARGETS_DIR` compile definition pointing to their build-tree location.

### Test tags

- `[process]` — process lifecycle tests (launch, attach, resume)
- `[register]` — register read/write tests
- `[breakpoint]` — breakpoint creation, lookup, removal, and address-hit tests
- `[memory]` — memory read/write tests
- `[watchpoint]` — hardware watchpoint tests, including the `anti_debugger` target that defeats a checksum check by combining a `read_write` watchpoint with a later software breakpoint
- `[syscall]` — syscall name/id mapping (`syscall_id_to_name`, `syscall_name_to_id`)
- `[catchpoint]` — syscall catchpoint behavior (`syscall_catch_policy` filtering, entry/exit pairing)

### Key design patterns

- **`process` factory pattern**: Private constructor; clients must use `process::launch()` or `process::attach()`. `launch()` sets `terminate_on_end_=true` (kills child on destruction); `attach()` sets it to `false` (detaches only). `launch()` uses a `pipe` with `O_CLOEXEC` to propagate exec errors from child to parent as exceptions. `launch(path, false)` launches without tracing (no `PTRACE_TRACEME`), used in tests that separately `attach()`.
- **`registers` ownership**: Only constructible by `process` (`friend` class). Stores a `user` struct from `sys/user.h`; values are `std::variant` across sizes/types. `read_by_id_as<T>()` and `write_by_id()` are convenience wrappers over `read()`/`write()` that take `register_info`.
- **`error` factory pattern**: Inherits `std::runtime_error`. Private constructor with static `send()` and `send_errno()` factories. `send_errno()` appends `strerror(errno)`.
- **X-macro register table** (`detail/registers.inc`): Included twice in `register_info.hpp` — once to generate the `register_id` enum, once to populate `g_register_infos[]`. To add a register, add one line in `registers.inc`. Lookup helpers: `register_info_by_id()`, `register_info_by_name()`, `register_info_by_dwarf()`.
- **Register read/write flow**: `wait_on_signal()` triggers `read_all_registers()`, populating `registers::data_` via `PTRACE_GETREGS` (GPRs), `PTRACE_GETFPREGS` (FPRs), and `PTRACE_PEEKUSER` (debug registers dr0–dr7). Writing routes through `process::write_gprs()` / `write_fprs()` / `write_user_area()` depending on register type.
- **Stop-reason augmentation**: `stop_reason` carries an optional `trap_type` (`single_step`, `software_break`, `hardware_break`, `syscall`, `unknown`) and an optional `syscall_information`. `wait_on_signal()` calls `augment_stop_reason()`, which issues `PTRACE_GETSIGINFO` and maps `siginfo_t::si_code` → `trap_type` (`TRAP_TRACE` → single step, `SI_KERNEL` → software break — x64 uses `SI_KERNEL` instead of `TRAP_BRKPT` for `int3`, `TRAP_HWBKPT` → hardware break, `SIGTRAP|0x80` → syscall stop). After augmentation, `wait_on_signal()` only rewinds PC for *software* breakpoints (and verifies the site is still enabled); for hardware traps it calls `get_current_hardware_stoppoint()` and, if the firing slot belongs to a watchpoint, refreshes that watchpoint's `data_`/`previous_data_`; for syscall traps it calls `maybe_resume_from_syscall()` to apply the catch policy.
- **Identifying which hardware slot fired**: `process::get_current_hardware_stoppoint()` reads DR6, finds the lowest set bit with `__builtin_ctzll` (the slot index 0–3), reads the matching DR0–DR3 register for the address, then looks it up in either `breakpoint_sites_` or `watchpoints_` and returns a `std::variant<breakpoint_site::id_type, watchpoint::id_type>` (index 0 = hardware breakpoint site, index 1 = watchpoint). The CLI's `get_sigtrap_info()` uses this variant to format the stop message ("breakpoint N" vs "watchpoint N" with old/new values).
- **Breakpoints**: `breakpoint_site` represents a breakpoint at a `virt_addr` (software by default). Private constructor; only `process` can create them (via `friend`). Each site gets a unique auto-incrementing `id_type` id. For software sites, stores the original byte (`saved_data_`) displaced by the `int3` opcode; `enable()`/`disable()` toggle patching. Sites also carry `is_hardware_` / `is_internal_` flags plus a `hardware_register_index_` (DR0–DR3 slot). `stoppoint_collection<T>` is a generic header-only template container (constrained by `stoppoint_concept` C++20 concept) that manages stoppoints by id or address; same container will hold watchpoints. `process` stores breakpoints in `stoppoint_collection<breakpoint_site>` and exposes `create_breakpoint_site(virt_addr, hardware=false, internal=false)` to add them.
- **Hardware breakpoints**: `process::set_hardware_breakpoint(id, addr)` delegates to `set_hardware_stoppoint(addr, stoppoint_mode::execute, 1)`, which finds a free DR0–DR3 slot from DR7, writes the address, and encodes mode/size bits. `stoppoint_mode` (`write`, `read_write`, `execute`) lives in `types.hpp`. `read_memory_without_traps` only patches over *software* sites — hardware sites don't modify code bytes and are skipped.
- **Watchpoints**: `watchpoint` (in `include/libgsdb/watchpoint.hpp`) mirrors `breakpoint_site`'s shape — private constructor with `process` as `friend`, deleted copy, auto-incrementing `id_type`, `enable()`/`disable()`, `at_address`/`in_range`. Stored in a parallel `stoppoint_collection<watchpoint>` on `process`. Created via `process::create_watchpoint(addr, stoppoint_mode, size)` where `mode` is `write`/`read_write` (size 1, 2, 4, or 8 — x64 requires natural alignment) or `execute` (which is really a hw breakpoint slot). Under the hood watchpoints share the same DR0–DR3 pool as hardware breakpoints, so the two are jointly capped at 4 active. The `[watchpoint]` test uses the `anti_debugger` target: a `read_write` watchpoint fires when the target's checksum routine *reads* the protected function's bytes, then the test steps past the read and *only then* installs a software breakpoint — the int3 patch happens after the checksum has already consumed the original byte.
- **Watchpoint value tracking**: each `watchpoint` carries `data_` and `previous_data_` (`std::uint64_t`). `update_data()` re-reads `size_` bytes via `process_->read_memory()` and shifts the prior value into `previous_data_` (via `std::exchange`). It runs once in the constructor (so the first fire has a meaningful "old value") and again from `wait_on_signal()` whenever a hardware trap maps back to this watchpoint. The CLI compares the two to print either a single `Value: 0xNN` (read_write hit with unchanged data) or `Old value` / `New value` lines.
- **Pipe-based test communication**: Register tests use `gsdb::pipe` with `process::launch(path, true, channel.get_write())` to redirect the target's stdout. The assembly targets write register values to stdout, and tests read them back via `channel.read()` for verification. `pipe`'s constructor takes a `close_on_exec` flag: register tests pass `true` (write end is closed by the child's `execve`, so parent reads see EOF cleanly); the `anti_debugger` watchpoint test passes `false` because the target must inherit the write fd across `exec` to keep writing to the pipe.
- **Syscall catchpoints**: opt-in via `process::set_syscall_catch_policy()` with one of `syscall_catch_policy::catch_all()`, `catch_none()` (default), or `catch_some({ids…})`. When the policy is non-`none`, `resume()` issues `PTRACE_SYSCALL` instead of `PTRACE_CONT`, so the inferior stops once on each syscall *entry* and once on *exit*. `PTRACE_O_TRACESYSGOOD` (set once after the initial halt in `launch()`) makes the kernel deliver these as `SIGTRAP | 0x80`, distinguishing them from real `SIGTRAP`s. `stop_reason::syscall_info` carries `{id, entry, args[6] | ret}` (an anonymous union — `args` on entry, `ret` on exit). `process` tracks pairing with `expecting_syscall_exit_`; `maybe_resume_from_syscall()` consults the policy and transparently resumes (so a filtered-out syscall doesn't surface to the CLI). The X-macro table `src/include/syscalls.inc` (generated from `asm/unistd_64.h` — see README) drives both `syscall_id_to_name` (switch) and `syscall_name_to_id` (unordered_map).
- **Disassembler**: `disassembler` wraps the Zydis library to decode x86-64 instructions from process memory. Takes a `process&` reference. `disassemble(n, optional_address)` reads `n * 15` bytes (max x64 instruction size) via `read_memory_without_traps` (which replaces int3 bytes with saved originals from active breakpoint sites), decodes them with `ZydisDisassembleATT`, and returns a vector of `instruction{virt_addr, string}`. Defaults to disassembling from the current PC.
- **Utility headers**: `bit.hpp` provides `from_bytes<T>`, `as_bytes`, `to_byte128/64` for safe type-punning via `memcpy`. `parse.hpp` provides `to_integral<T>`, `to_float<T>`, and `parse_vector<N>` for parsing user input (register values). `types.hpp` defines `virt_addr` (strong-typed virtual address) and `byte64`/`byte128` aliases.
- The library is being refactored to move debugger primitives out of `tools/gsdb.cpp` into `libgsdb`.

## Running the Debugger

```bash
# Launch a program under the debugger
./build/tools/gsdb <program>

# Attach to a running process by PID
./build/tools/gsdb -p <pid>
```

REPL commands (all prefix-matched, so `c` works for `continue`):
- `continue` — resume the process
- `step` — step over a single instruction
- `register read [<name>|all]` — read registers (defaults to GPRs; `all` shows all types)
- `register write <name> <value>` — write a register
- `breakpoint set <address>` / `list` / `enable <id>` / `disable <id>` / `delete <id>`
- `watchpoint set <address> <mode> <size>` / `list` / `enable <id>` / `disable <id>` / `delete <id>` — `mode` is `write`, `rw`, or `execute`; `size` is 1/2/4/8
- `memory read <address> [<n-bytes>]` — read memory (default 32 bytes, displayed in 16-byte rows)
- `memory write <address> <bytes>` — write bytes to memory
- `disassemble [-c <count>] [-a <address>]` — disassemble instructions (default: 5 instructions from current PC)
- `catchpoint syscall [none | <comma-separated-list>]` — set the syscall catch policy. No argument catches all; `none` disables; the list accepts numeric IDs or names (e.g. `write,read`)
- `help [command]` — show help

`continue` and `step` automatically print 5 disassembled instructions at the stop point. Empty input repeats the last command. When the inferior stops on `SIGTRAP`, the stop message annotates the cause via `get_sigtrap_info()`: `(breakpoint N)` for software / hardware breakpoints, `(single step)` after `step`, or `(watchpoint N)` followed by either `Value: …` or `Old value: … / New value: …` for watchpoint hits.

## Dependencies

- **libedit** - found via pkg-config, linked to the CLI tool only (provides `readline`-compatible line editing)
- **Zydis** - x86/x86-64 disassembler library, found via `find_package(zydis CONFIG)`, linked to `libgsdb` (used by the `disassembler` class)
- **Catch2** (v3) - test framework, found via `find_package(Catch2 CONFIG)`
- **GTest** - also found in root CMakeLists.txt (`find_package(GTest REQUIRED)`) but not used by any test target; tests use Catch2 exclusively. This is a leftover that could be removed.

## Troubleshooting

### Clangd false positives with libstdc++

Clangd may report spurious errors like `no type named '_Tp_alloc_type'` in `std::vector` or other STL containers. This is a clangd-vs-GCC-libstdc++ mismatch — Clang's AST can't always resolve GCC's internal typedefs. The code compiles fine with GCC.

## Code Style

- `.clang-format`: Google style, 4-space indentation
- All code lives in the `gsdb` namespace
- Compiler flags: `-Wall -Wfatal-errors -Wextra -Werror -g -O1` (warnings are errors)
- Resource-owning types use private constructors with `friend` access (see `process`, `registers`, `breakpoint_site`)
- Copy operations are deleted on resource types; move may be allowed where appropriate
