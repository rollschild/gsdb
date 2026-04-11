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

# Generate compile_commands.json (for clangd)
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ln -s build/compile_commands.json .
```

## Architecture

```
 include/libgsdb/          (public headers)
 src/                      (library implementation)
        ↓
    [libgsdb.a]  (CMake target: gsdb::libgsdb)
        ↓                          ↓
 + PkgConfig::libedit       + Catch2::Catch2WithMain
        ↓                          ↓
 tools/gsdb                 test/tests
 (CLI binary)               (test binary)
```

- **`src/`** → `libgsdb.a` (static library, CMake target `gsdb::libgsdb`). Public headers in `include/libgsdb/`, private headers in `src/include/` (currently empty). OUTPUT_NAME override prevents `liblibgsdb.a`.
- **`tools/`** → `gsdb` CLI. REPL via libedit, command parsing, attach logic (fork+exec with `PTRACE_TRACEME` or `PTRACE_ATTACH` to PID via `-p`).
- **`test/`** → Catch2 v3 tests (`Catch2::Catch2WithMain` supplies `main()`). Debuggee target binaries live in `test/targets/` (C++ and assembly, assembly uses `-pie`). Tests reference them via the `TARGETS_DIR` compile definition pointing to their build-tree location.

### Test tags

- `[process]` — process lifecycle tests (launch, attach, resume)
- `[register]` — register read/write tests

### Key design patterns

- **`process` factory pattern**: Private constructor; clients must use `process::launch()` or `process::attach()`. `launch()` sets `terminate_on_end_=true` (kills child on destruction); `attach()` sets it to `false` (detaches only). `launch()` uses a `pipe` with `O_CLOEXEC` to propagate exec errors from child to parent as exceptions. `launch(path, false)` launches without tracing (no `PTRACE_TRACEME`), used in tests that separately `attach()`.
- **`registers` ownership**: Only constructible by `process` (`friend` class). Stores a `user` struct from `sys/user.h`; values are `std::variant` across sizes/types. `read_by_id_as<T>()` and `write_by_id()` are convenience wrappers over `read()`/`write()` that take `register_info`.
- **`error` factory pattern**: Inherits `std::runtime_error`. Private constructor with static `send()` and `send_errno()` factories. `send_errno()` appends `strerror(errno)`.
- **X-macro register table** (`detail/registers.inc`): Included twice in `register_info.hpp` — once to generate the `register_id` enum, once to populate `g_register_infos[]`. To add a register, add one line in `registers.inc`. Lookup helpers: `register_info_by_id()`, `register_info_by_name()`, `register_info_by_dwarf()`.
- **Register read/write flow**: `wait_on_signal()` triggers `read_all_registers()`, populating `registers::data_` via `PTRACE_GETREGS` (GPRs), `PTRACE_GETFPREGS` (FPRs), and `PTRACE_PEEKUSER` (debug registers dr0–dr7). Writing routes through `process::write_gprs()` / `write_fprs()` / `write_user_area()` depending on register type.
- **Breakpoints**: `breakpoint_site` represents a software breakpoint at a `virt_addr`. Private constructor; only `process` can create them (via `friend`). Each site gets a unique auto-incrementing `id_type` id. Stores the original byte (`saved_data_`) displaced by the `int3` opcode. `enable()`/`disable()` toggle patching. `stoppoint_collection<T>` is a generic header-only template container (constrained by `stoppoint_concept` C++20 concept) that manages stoppoints by id or address; will also be used for watchpoints/hardware breakpoints later. `process` stores breakpoints in `stoppoint_collection<breakpoint_site>` and exposes `create_breakpoint_site(virt_addr)` to add them.
- **Pipe-based test communication**: Register tests use `gsdb::pipe` with `process::launch(path, true, channel.get_write())` to redirect the target's stdout. The assembly targets write register values to stdout, and tests read them back via `channel.read()` for verification.
- **Utility headers**: `bit.hpp` provides `from_bytes<T>`, `as_bytes`, `to_byte128/64` for safe type-punning via `memcpy`. `parse.hpp` provides `to_integral<T>`, `to_float<T>`, and `parse_vector<N>` for parsing user input (register values). `types.hpp` defines `virt_addr` (strong-typed virtual address) and `byte64`/`byte128` aliases.
- The library is being refactored to move debugger primitives out of `tools/gsdb.cpp` into `libgsdb`.

## Running the Debugger

```bash
# Launch a program under the debugger
./build/tools/gsdb <program>

# Attach to a running process by PID
./build/tools/gsdb -p <pid>
```

The REPL supports `continue` and `register` commands (prefix-matched, so `c` works for continue). `help` shows available commands. Empty input repeats the last command.

## Dependencies

- **libedit** - found via pkg-config, linked to the CLI tool only (provides `readline`-compatible line editing)
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
