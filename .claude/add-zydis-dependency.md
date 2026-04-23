# Proposal: Add Zydis as a dependency for x86-64 disassembly

Zydis 4.1.1 is available in nixpkgs as `zydis` (with its dependency `zycore`).
CMake target: `Zydis::Zydis`, found via `find_package(zydis CONFIG REQUIRED)`.

## Changes

### 1. `flake.nix` — add Zydis to the dev shell

In `buildInputs`, add `zydis` (it's a linked library, consistent with `libedit`):

```nix
buildInputs = with pkgs; [
  libedit
  curl
  zydis    # <-- add this
];
```

### 2. `CMakeLists.txt` — find the package

In the Dependencies section (around line 86), add:

```cmake
find_package(zydis CONFIG REQUIRED)
```

### 3. `src/CMakeLists.txt` — link Zydis to libgsdb

Add after the `target_include_directories` block:

```cmake
target_link_libraries(libgsdb PUBLIC Zydis::Zydis)
```

`PUBLIC` so that both `tools/gsdb` and `test/tests` (which link against `gsdb::libgsdb`) inherit the Zydis headers and library.

### 4. Usage

After the above, any source in `src/`, `tools/`, or `test/` can:

```cpp
#include <Zydis/Zydis.h>

// Example: disassemble a buffer of raw bytes
ZydisDecoder decoder;
ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

ZydisFormatter formatter;
ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);

ZydisDecodedInstruction instruction;
ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
char buffer[256];

while (ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, data, length, &instruction, operands))) {
    ZydisFormatterFormatInstruction(&formatter, &instruction, operands,
        instruction.operand_count_visible, buffer, sizeof(buffer), address, ZYAN_NULL);
    // buffer now contains e.g. "mov rax, [rbp-0x8]"
    data += instruction.length;
    length -= instruction.length;
    address += instruction.length;
}
```

### Notes

- After editing `flake.nix`, re-enter the dev shell with `nix develop` to pick up Zydis.
- Zydis is a static library (`libZydis.a`) — no runtime dependency.
- The Nix package defines `ZYDIS_STATIC_BUILD` automatically via the CMake target's `INTERFACE_COMPILE_DEFINITIONS`.
