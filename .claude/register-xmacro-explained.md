# X-Macro Pattern for Register Definitions

## Overview

`registers.inc` + `register_info.hpp` use an **X-macro** (include-file macro) pattern: define the register table **once** in `registers.inc`, then `#include` it multiple times with different definitions of `DEFINE_REGISTER` to generate different C++ constructs from the same data.

## How it works

`registers.inc` is **not** a normal header — it's a raw list of `DEFINE_REGISTER(name, dwarf_id, size, offset, type, format)` invocations. It expects `DEFINE_REGISTER` to already be `#define`d before inclusion, and errors out if it isn't.

In `register_info.hpp`, it's included **twice** with different macro definitions:

1. **Generates the `register_id` enum:**
   ```cpp
   #define DEFINE_REGISTER(name, ...) name
   ```
   Each register entry becomes just its name → `enum class register_id { rax, rdx, rcx, ... };`

2. **Generates the `g_register_infos[]` array:**
   ```cpp
   #define DEFINE_REGISTER(name, dwarf_id, size, offset, type, format) \
       {register_id::name, #name, dwarf_id, size, offset, type, format}
   ```
   Each entry becomes a `register_info` struct initializer. `#name` stringifies the name for the `string_view` field.

## Helper macros in `registers.inc`

These reduce repetition for each register category:

- **`GPR_OFFSET(reg)`** — Computes the byte offset of a general-purpose register within the Linux `user` struct (what `ptrace` returns with `PTRACE_PEEKUSER`).
- **`DEFINE_GPR_64/32/16/8H/8L`** — Shortcuts for GPRs at different widths. Sub-registers (32/16/8-bit) share the same offset as their 64-bit parent (with `8H` adding +1 for the high byte like `ah`). They have `dwarf_id = -1` since DWARF only numbers the full 64-bit registers. Their type is `sub_gpr` rather than `gpr`.
- **`FPR_OFFSET/FPR_SIZE`** — Same idea for floating-point registers, offset within `user_fpregs_struct`.
- **`DEFINE_FPR`** — General floating-point control registers (`fcw`, `mxcsr`, etc.).
- **`DEFINE_FP_ST`** — x87 stack registers (`st0`-`st7`), DWARF IDs 33-40, 16 bytes each, `long_double` format.
- **`DEFINE_FP_MM`** — MMX registers (`mm0`-`mm7`), DWARF IDs 41-48, 8 bytes overlapping `st_space`.
- **`DEFINE_FP_XMM`** — SSE registers (`xmm0`-`xmm15`), DWARF IDs 17-32, 16 bytes, `vector` format.
- **`DEFINE_DR`** — Hardware debug registers (`dr0`-`dr7`), no DWARF IDs, used for hardware breakpoints/watchpoints.

## Why this pattern

It guarantees the enum and the info array stay perfectly in sync — you can't add a register to one without the other, since both are generated from the same source. Adding a new register is a one-liner.
