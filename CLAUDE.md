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
include/libgsdb/
  process.hpp        (public: process class)
  error.hpp          (public: error class)
  pipe.hpp           (public: pipe class)
  registers.hpp      (public: registers class)
  register_info.hpp  (public: register metadata)
  types.hpp          (public: byte64/byte128 aliases)
  bit.hpp            (public: byte-level helpers)
  detail/registers.inc  (X-macro register table)
        ↓ PUBLIC include path
 src/process.cpp, pipe.cpp → [libgsdb.a]
        ↓                          ↓
 gsdb::libgsdb              gsdb::libgsdb
 + PkgConfig::libedit       + Catch2::Catch2WithMain
        ↓                          ↓
 tools/gsdb                 test/tests
 (CLI binary)               (test binary)
```

- **`src/`** - `libgsdb`: Core library built as a static library. Public headers in `include/libgsdb/`, private headers go in `src/include/` (configured in CMake but currently empty). The CMake target is `gsdb::libgsdb`. Output file is `libgsdb.a` (OUTPUT_NAME override prevents `liblibgsdb.a`).
- **`tools/`** - `gsdb`: CLI executable linking against `gsdb::libgsdb` and `PkgConfig::libedit`. Contains the REPL loop (`readline`/`libedit`), command parsing, and the `attach` logic (both fork+exec with `PTRACE_TRACEME` and `PTRACE_ATTACH` to an existing PID via `-p`).
- **`test/`** - Unit tests using Catch2 v3 (`Catch2::Catch2WithMain` supplies `main()`). Test helper binaries (debuggee targets) live in `test/targets/` and are compiled as separate executables (C++ like `run_endlessly.cpp` and assembly like `reg_write.s`). Tests reference them via the `TARGETS_DIR` compile definition, which points to the build-tree location of these binaries. `process::launch(path, false)` launches without tracing (no `PTRACE_TRACEME`), used in tests that separately `attach()`. Assembly targets use `-pie` (position-independent executable).

### Key design patterns

- **`process` class** (`include/libgsdb/process.hpp`): Encapsulates process lifecycle (launch/attach, resume, wait_on_signal) with a `process_state` enum. Uses a private constructor — clients must use the static `process::launch()` or `process::attach()` factory methods. `launch()` sets `terminate_on_end_=true` (kills child on destruction); `attach()` sets it to `false` (detaches only). `launch()` uses a `pipe` with `O_CLOEXEC` to propagate exec errors from child to parent as exceptions.
- **`registers` class** (`include/libgsdb/registers.hpp`): Wraps register read/write access. Only constructible by `process` (private constructor, `friend process`). Stores a `user` struct (from `sys/user.h`) and uses `std::variant` to represent register values across different sizes/types. Provides `read_by_id_as<T>()` and `write_by_id()` convenience methods on top of the raw `read()`/`write()` that take `register_info`.
- **`error` class** (`include/libgsdb/error.hpp`): Exception type inheriting `std::runtime_error` with static `send()` and `send_errno()` factory methods (private constructor). `send_errno()` appends `strerror(errno)` automatically.
- **X-macro register table** (`include/libgsdb/detail/registers.inc`): Defines all x86-64 registers (GPR, sub-GPR, FPR/x87/SSE, debug) using X-macros. Included twice in `register_info.hpp` — once to generate the `register_id` enum, once to populate the `g_register_infos[]` array. Each register entry specifies its DWARF number, size, offset into the `user` struct (for `ptrace`), type, and display format. To add a new register, add a single line in `registers.inc` and both the enum and info array stay in sync automatically. Lookup helpers: `register_info_by_id()`, `register_info_by_name()`, `register_info_by_dwarf()`.
- **Register read/write flow**: When a process stops (`wait_on_signal()`), `read_all_registers()` is called automatically, populating the `registers::data_` (`user` struct) via three ptrace calls: `PTRACE_GETREGS` (GPRs), `PTRACE_GETFPREGS` (FPRs), and `PTRACE_PEEKUSER` (debug registers dr0–dr7). Writing back uses `registers::write()`, which routes through `process::write_gprs()` / `write_fprs()` / `write_user_area()` depending on register type.
- The library is being refactored to move debugger primitives out of `tools/gsdb.cpp` into `libgsdb`.

## Running the Debugger

```bash
# Launch a program under the debugger
./build/tools/gsdb <program>

# Attach to a running process by PID
./build/tools/gsdb -p <pid>
```

The REPL supports `continue` (prefix-matched, so `c` works). Empty input repeats the last command.

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
