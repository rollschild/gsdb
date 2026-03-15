# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Note to Claude

Do _NOT_ directly change source files, ever. Save any files/plans under `.claude/` directory at the project's root directory. Do _NOT_ touch my `$HOME` directory, ever.

## Project

gsdb is a C++23 project built with CMake. It uses a Nix flake (`flake.nix`) for development environment setup.

## Build Commands

```bash
# Configure (out-of-source build required)
cmake -S . -B build

# Build everything
cmake --build build

# Build a specific target
cmake --build build --target gsdb      # CLI tool
cmake --build build --target libgsdb   # library
cmake --build build --target tests     # test binary

# Run tests
cd build && ctest
# Or run the test binary directly:
./build/test/tests
```

## Architecture

The project has three layers:

- **`src/`** - `libgsdb`: Core library. Public headers are in `include/libgsdb/`, private headers in `src/include/`. Built as a static library with namespace `gsdb::`. Other targets link against `gsdb::libgsdb`.
- **`tools/`** - `gsdb`: CLI executable that links against `libgsdb` and `libedit`.
- **`test/`** - Unit tests using Catch2 (with `Catch2WithMain` providing the entry point).

## Dependencies

- **libedit** - found via pkg-config, linked to the CLI tool
- **Catch2** (v3) - test framework
- **GTest** - also available but Catch2 is used for the test target

## Code Style

- `.clang-format`: Google style with 4-space indentation
- All code lives in the `gsdb` namespace
- Compiler warnings are strict: `-Wall -Wfatal-errors -Wextra -Werror`
