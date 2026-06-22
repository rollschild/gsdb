# DWARF Line Table

A **line table** (line number program/table) is the part of DWARF debug info that
maps machine code addresses back to source code locations — file, line, and
column. It lives in the `.debug_line` section of the ELF binary.

## The core problem it solves

The compiler turns source into machine instructions, scrambling the 1:1
relationship. A single source line can become many instructions, instructions
get reordered/interleaved by optimization, and inlining mixes code from different
files. When the debugger stops at some `virt_addr` (a PC value), it needs to
answer: *"what source line is this?"* — and the reverse: *"the user set a
breakpoint at `main.cpp:42`, what address is that?"* The line table is the
bidirectional mapping that makes both possible.

## What's in it conceptually

Logically it's a big sorted table, one row per address where source position
info changes:

```
address       file        line   col   is_stmt   end_sequence
0x401120      main.cpp    10     0     yes        no
0x401128      main.cpp    11     5     yes        no
0x401135      main.cpp    12     0     yes        no
0x401140      util.cpp    3      0     yes        no    (inlined)
...
0x401200      -           -      -     -          yes   (end of this run)
```

Each row says "starting at this address, you're on this source line, until the
next row's address."

## Why it's tricky to parse

That table would be huge if stored literally, so DWARF doesn't store rows — it
stores a **bytecode program** that, when executed by a tiny state machine,
*generates* the rows. The `.debug_line` section contains:

- A **header**: address size, the list of file names and include directories,
  and the parameters of the state machine (`line_base`, `line_range`,
  `opcode_base`, `minimum_instruction_length`, etc.).
- A **program**: a stream of opcodes that nudge a virtual machine's registers
  (`address`, `file`, `line`, `column`, `is_stmt`, `end_sequence`…). There are
  three opcode kinds:
  - **Special opcodes** (the bulk) — a single byte that advances *both* address
    and line by a packed amount, then emits a row. This is the compression
    trick.
  - **Standard opcodes** — `DW_LNS_advance_pc`, `DW_LNS_advance_line`,
    `DW_LNS_copy`, `DW_LNS_set_file`, etc.
  - **Extended opcodes** — `DW_LNE_end_sequence` (emits a terminating row,
    resets the machine), `DW_LNE_set_address` (loads an absolute address —
    relocated).

You run this program start to finish to reconstruct the full sorted table, then
index it for lookups.

## Where it fits in gsdb

The current `dwarf` parser (`src/dwarf.cpp`) handles
`.debug_info`/`.debug_abbrev`/`.debug_str`/`.debug_ranges` — the DIE tree, which
gives functions and their address ranges. The line table is the **next layer
down**: from "PC is in `main`" to "PC is at `main.cpp:42`." It is what's needed
for source-level stepping (`step over`, `step into`, `step out` that respect
source lines rather than machine instructions) and for
`breakpoint set main.cpp:42`.

It is a natural addition: a `line_table` class that reads `.debug_line` (per
compile unit — the CU's root DIE has a `DW_AT_stmt_list` attribute giving the
offset into `.debug_line`), runs the state machine, and exposes
`get_entry_by_address(file_addr)` and `get_entries_by_line(file, line)`.
