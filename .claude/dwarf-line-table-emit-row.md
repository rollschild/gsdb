# DWARF Line Tables: What "Emit Row" Means

In DWARF line-number tables, "emit row" is a term from the **line number
program** — the bytecode that DWARF uses to build the line table compactly.

## The core idea

The line table is conceptually a big matrix. Each **row** maps a machine
address to a source location:

| address | file | line | column | is_stmt | end_sequence | ... |
|---------|------|------|--------|---------|--------------|-----|
| 0x1140  | 1    | 5    | 0      | yes     | no           |
| 0x1148  | 1    | 6    | 0      | yes     | no           |
| 0x1155  | 1    | 7    | 12     | yes     | no           |

Storing this matrix literally would be huge. So instead DWARF stores a tiny
**program** of opcodes that a state machine executes. The state machine holds a
set of **registers** (`address`, `file`, `line`, `column`, `is_stmt`,
`op_index`, `end_sequence`, etc.) that together describe *one row*.

## What "emit a row" means

As the program runs, the opcodes mutate those registers —
`DW_LNS_advance_pc` bumps the address, `DW_LNS_advance_line` bumps the line,
`DW_LNS_set_column` sets the column, and so on. **Mutating registers does not
produce output.** The current register values are just a working "current row."

Certain opcodes — the **special opcodes**, `DW_LNS_copy`, and
`DW_LNE_end_sequence` — say: *take a snapshot of the register state right now
and append it to the line table as a finished row.* **That snapshot is the
emitted row.** After emitting, the state machine keeps running from the same
register values to build the next row.

So "emit row" = "append the current snapshot of
`(address, file, line, column, flags…)` to the output table."

## Which row is emitted?

The one currently held in the state-machine registers at the moment an emitting
opcode fires. Concretely the most common case is a **special opcode**, which
does three things in one byte:

1. advance `address` by some amount,
2. advance `line` by some (possibly negative) amount,
3. **emit a row** with the new values.

This is why special opcodes are so dense: the most common transition in real
code is "move forward a few bytes of machine code and forward one source line,"
which is exactly one byte that advances both and emits.

The other emitters:

- `DW_LNS_copy` — emit a row **without** advancing address/line (used when you
  need to record a row but the special-opcode encoding doesn't fit), then clears
  the `discriminator`/`basic_block`/etc. flags.
- `DW_LNE_end_sequence` — sets `end_sequence = true`, **emits that final row**,
  then resets the entire state machine to defaults. This terminal row's address
  marks the *end* (one-past-the-last) of the address range for that sequence; it
  has no real source line, it just bounds the previous row.

## Why it matters for a debugger

When you (gsdb) want to answer "what source line is address X on?", you run the
line program to materialize these rows, then find the row whose address is the
greatest one `≤ X` (and whose sequence isn't ended). Each emitted row defines
the start of an address range that extends until the *next* emitted row. So
"emit row" is precisely the act of laying down one of those address-range
boundary points that the lookup later binary-searches over.
