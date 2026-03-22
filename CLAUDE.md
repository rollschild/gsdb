# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Note to Claude

Do _NOT_ directly change source files, ever, except for `CLAUDE.md`. For `CLAUDE.md` you are free to make any changes on your own. Save any plans/proposals under `.claude/` directory at the project's root directory. Do _NOT_ touch my `$HOME` directory, ever.

## Project

gsdb is a Linux debugger written from scratch in C++23, built with CMake. It uses a Nix flake (`flake.nix`) for development environment setup. The debugger uses `ptrace` for process control and targets ELF binaries with DWARF debug info.

## Environment Setup

```bash
# Enter the Nix dev shell (provides GCC, CMake, Catch2, libedit, etc.)
nix develop
```

The flake uses GCC (not Clang) and LLVM 18 packages. The dev shell automatically switches to zsh with starship prompt.

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
  process.hpp   (public: process class)
  error.hpp     (public: error class)
  pipe.hpp      (public: pipe class)
      ↓ PUBLIC include path
 src/process.cpp, pipe.cpp → [libgsdb.a]
      ↓                          ↓
 gsdb::libgsdb              gsdb::libgsdb
 + PkgConfig::libedit       + Catch2::Catch2WithMain
      ↓                          ↓
 tools/gsdb                 test/tests
 (CLI binary)               (test binary)
```

- **`src/`** - `libgsdb`: Core library built as a static library. Public headers in `include/libgsdb/`, private headers in `src/include/`. The CMake target is `gsdb::libgsdb`. Output file is `libgsdb.a` (OUTPUT_NAME override prevents `liblibgsdb.a`).
- **`tools/`** - `gsdb`: CLI executable linking against `gsdb::libgsdb` and `PkgConfig::libedit`. Contains the REPL loop (`readline`/`libedit`), command parsing, and the `attach` logic (both fork+exec with `PTRACE_TRACEME` and `PTRACE_ATTACH` to an existing PID via `-p`).
- **`test/`** - Unit tests using Catch2 v3 (`Catch2::Catch2WithMain` supplies `main()`). Test helper binaries (debuggee targets) live in `test/targets/` and are compiled as separate executables. Tests reference them via the `TARGETS_DIR` compile definition, which points to the build-tree location of these binaries.

### Key design patterns

- **`process` class** (`include/libgsdb/process.hpp`): Encapsulates process lifecycle (launch/attach, resume, wait_on_signal) with a `process_state` enum. Uses a private constructor — clients must use the static `process::launch()` or `process::attach()` factory methods. `launch()` sets `terminate_on_end_=true` (kills child on destruction); `attach()` sets it to `false` (detaches only).
- **`pipe` class** (`include/libgsdb/pipe.hpp`): RAII wrapper around Linux `pipe2()`. Used internally by `process::launch()` to communicate exec errors from the child back to the parent. Supports `O_CLOEXEC` so the pipe auto-closes on successful `exec`.
- **`error` class** (`include/libgsdb/error.hpp`): Exception type inheriting `std::runtime_error` with static `send()` and `send_errno()` factory methods (private constructor). `send_errno()` appends `strerror(errno)` automatically.
- The library is being refactored to move debugger primitives out of `tools/gsdb.cpp` into `libgsdb`.

## Dependencies

- **libedit** - found via pkg-config, linked to the CLI tool only (provides `readline`-compatible line editing)
- **Catch2** (v3) - test framework, found via `find_package(Catch2 CONFIG)`
- **GTest** - available in the Nix flake and found by CMake, but not currently used by any target
- **Boost**, **curl** - available in the Nix flake but not currently used by any CMake target

## Troubleshooting

### Clangd false positives with libstdc++

Clangd may report spurious errors like `no type named '_Tp_alloc_type'` in `std::vector` or other STL containers. This is a clangd-vs-GCC-libstdc++ mismatch — Clang's AST can't always resolve GCC's internal typedefs. The code compiles fine with GCC. Updating clangd to match the LLVM 18 in the flake can help.

## Code Style

- `.clang-format`: Google style, 4-space indentation
- All code lives in the `gsdb` namespace
- Compiler flags: `-Wall -Wfatal-errors -Wextra -Werror -g -O1` (warnings are errors)
