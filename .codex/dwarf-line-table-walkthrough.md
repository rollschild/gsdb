# DWARF Line Table Walkthrough

This document explains the current line-table implementation in `gsdb`: what it
parses, how the data model is shaped, how iteration runs the DWARF line-number
program, and where the design has sharp edges.

The implementation is mainly in:

- `include/libgsdb/dwarf.hpp`
- `src/dwarf.cpp`
- `include/libgsdb/detail/dwarf.h`
- `test/tests.cpp`

## Code Map

```text
include/libgsdb/dwarf.hpp
  line_table::file              stored file-table entries
  line_table                    parsed header + lazy line program
  line_table::entry             one emitted matrix row / VM register snapshot
  line_table::iterator          lazy DWARF line-program interpreter
  compile_unit::lines()         access to the compile unit's line table
  dwarf::line_entry_at_address  CU selection + address lookup
  die::location/file/line       DIE declaration/call-site source locations

src/dwarf.cpp
  parse_line_table_file         DWARF file-entry parser + path resolver
  parse_line_table              DWARF v4 line-table header parser
  path_ends_in                  relative path suffix match helper
  line_table::iterator methods  line-program execution
  get_entry_by_address          address-to-row lookup
  get_entries_by_line           source-line-to-row lookup

include/libgsdb/detail/dwarf.h
  DW_LNS_* and DW_LNE_* opcode constants

test/tests.cpp
  "Line table" Catch2 case over the hello_gsdb fixture
```

## 1. Mental Model

A DWARF line table maps machine-code address ranges back to source locations:

```text
address      file            line  column  flags
0x1139       hello.cpp       4     12      is_stmt
0x113d       hello.cpp       4     23      is_stmt
0x114c       hello.cpp       4     41      is_stmt, discriminator=1
0x1151       hello.cpp       4     41      !is_stmt
0x1153       hello.cpp       4     41      end_sequence
```

DWARF does not usually store that table directly. It stores a compact bytecode
program in `.debug_line`. A debugger executes the bytecode with a small state
machine. Each emitted state becomes one row in the logical line table.

In `gsdb`, the line table is lazy:

- the line-table header and file/directory tables are parsed eagerly when a
  compile unit is constructed;
- the row matrix is not materialized;
- `line_table::iterator` replays the line-number program and yields emitted rows
  one at a time.

```mermaid
flowchart LR
    DebugLine[".debug_line bytes"] --> Header["parse header"]
    Header --> Dirs["include_dirs"]
    Header --> Files["file_names"]
    Header --> Program["line program bytes"]
    Program --> Iterator["line_table::iterator"]
    Iterator --> Rows["logical rows"]
```

## 2. Object Ownership

Each `gsdb::dwarf` owns compile units. Each `compile_unit` owns one optional
`line_table`.

```mermaid
flowchart TB
    Elf["gsdb::elf<br/>mapped executable"] --> Dwarf["gsdb::dwarf"]
    Dwarf --> CU1["compile_unit"]
    Dwarf --> CU2["compile_unit"]
    CU1 --> Root1["root DIE"]
    CU1 --> LT1["line_table"]
    LT1 --> Data1["span over line program bytes"]
    LT1 --> Dirs1["include_dirs"]
    LT1 --> Files1["file_names"]
    LT1 --> Params1["default_is_stmt<br/>line_base<br/>line_range<br/>opcode_base"]
```

Relevant fields:

```text
line_table
|
+-- data_                span of line program bytes, after the header
+-- cu_                  owning compile unit context
+-- default_is_stmt_     initial state-machine value
+-- line_base_           special-opcode line delta base
+-- line_range_          special-opcode line delta modulus
+-- opcode_base_         first special opcode value
+-- include_dirs_        resolved directory table
+-- file_names_          resolved file table, mutable for DW_LNE_define_file

line_table::entry
|
+-- address              file_addr
+-- file_index           1-based file table index
+-- line
+-- column
+-- is_stmt
+-- basic_block_start
+-- end_sequence
+-- prologue_end
+-- epilogue_begin
+-- discriminator
+-- file_entry           pointer to file_names_[file_index - 1]
```

The implementation uses `file_addr`, not `virt_addr`, inside line-table rows.
Runtime addresses need to be converted through ELF load-bias logic before
line-table lookup.

## 3. Construction Flow

Line-table parsing is triggered while compile units are being parsed.

```mermaid
flowchart TD
    A["dwarf::dwarf(elf)"] --> B["parse_compile_units"]
    B --> C["parse_compile_unit"]
    C --> D["new compile_unit"]
    D --> E["compile_unit constructor"]
    E --> F["parse_line_table(*this)"]
    F --> G{"root DIE has<br/>DW_AT_stmt_list?"}
    G -- no --> H["return nullptr"]
    G -- yes --> I["read .debug_line<br/>at stmt_list offset"]
    I --> J["parse line header"]
    J --> K["resolve include dirs"]
    K --> L["resolve file table"]
    L --> M["store remaining bytes<br/>as line program span"]
    M --> N["line_table ready"]
```

The key path is:

```text
dwarf::dwarf
  -> parse_compile_units
  -> parse_compile_unit
  -> compile_unit::compile_unit
  -> parse_line_table
```

`parse_line_table()` reads `.debug_line`, using the compile unit root DIE's
`DW_AT_stmt_list` as the section offset.

## 4. Header Parsing

`parse_line_table()` supports DWARF v4 line tables with the assumptions already
used elsewhere in the project:

- DWARF32 length format;
- version `4`;
- minimum instruction length `1`;
- maximum operations per instruction `1`;
- standard opcode lengths matching the DWARF v4 defaults for opcodes 1-12.

The parser reads:

```text
unit_length
version
header_length
minimum_instruction_length
maximum_operations_per_instruction
default_is_stmt
line_base
line_range
opcode_base
standard_opcode_lengths
include_directories
file_names
line_program
```

The line-program span begins immediately after the terminating null byte of the
file-name table.

```mermaid
flowchart LR
    A[".debug_line at offset"] --> B["unit_length"]
    B --> C["version check"]
    C --> D["header fields"]
    D --> E["opcode length table"]
    E --> F["include dirs<br/>NUL-terminated list"]
    F --> G["file entries<br/>terminated by empty name"]
    G --> H["line program bytes"]
```

## 5. Directory And File Resolution

DWARF file entries contain:

```text
file name string
directory index
modification time
file length
```

`parse_line_table_file()` resolves each file to a path:

```mermaid
flowchart TD
    A["file entry"] --> B{"file path starts<br/>with / ?"}
    B -- yes --> C["use as absolute path"]
    B -- no --> D{"dir_index == 0 ?"}
    D -- yes --> E["compilation_dir / file"]
    D -- no --> F["include_dirs[dir_index - 1] / file"]
    C --> G["line_table::file"]
    E --> G
    F --> G
```

For relative lookup later, `get_entries_by_line()` accepts a relative path and
matches it as a suffix of the stored absolute path. That is handled by
`path_ends_in(lhs, rhs)`, which compares path components lexically:

```text
lhs = /repo/test/targets/hello_gsdb.cpp
rhs = targets/hello_gsdb.cpp

compare trailing components:
targets == targets
hello_gsdb.cpp == hello_gsdb.cpp
```

It does not inspect file contents. It also does not only compare the final
filename unless `rhs` itself contains only a filename.

## 6. Iterator Design

`line_table::begin()` constructs a `line_table::iterator`. The iterator stores:

```text
table_       pointer back to the line_table
current_     most recently emitted row
registers_   current DWARF line-state machine registers
pos_         current byte position in the line program
```

The iterator constructor initializes `registers_.is_stmt` from
`default_is_stmt_`, then immediately increments itself so `begin()` points at the
first emitted row.

```mermaid
flowchart TD
    A["line_table::begin()"] --> B["iterator(table)"]
    B --> C["pos_ = data_.begin()"]
    C --> D["registers_.is_stmt = default_is_stmt"]
    D --> E["++iterator"]
    E --> F{"instruction emitted row?"}
    F -- no --> G["execute next instruction"]
    G --> F
    F -- yes --> H["current_ is first row"]
```

`line_table::end()` returns a default/value-initialized iterator. Equality is
only `pos_ == rhs.pos_`, so the end sentinel is represented by `pos_ == nullptr`.

## 7. Line Program Execution

`operator++()` repeatedly calls `execute_instruction()` until an instruction
emits a row.

```mermaid
flowchart TD
    A["operator++"] --> B{"pos_ == data_.end()?"}
    B -- yes --> C["pos_ = nullptr<br/>return end"]
    B -- no --> D["execute_instruction"]
    D --> E{"emitted?"}
    E -- no --> D
    E -- yes --> F["attach file_entry<br/>file_names_[file_index - 1]"]
    F --> G["return iterator"]
```

The DWARF line program has three instruction classes.

```mermaid
flowchart TD
    A["read opcode byte"] --> B{"opcode kind"}
    B -- "1 <= opcode < opcode_base" --> C["standard opcode"]
    B -- "opcode == 0" --> D["extended opcode"]
    B -- "opcode >= opcode_base" --> E["special opcode"]
    C --> F{"DW_LNS_copy?"}
    F -- yes --> Row["emit current registers"]
    F -- no --> Mutate["mutate registers only"]
    D --> G{"DW_LNE_end_sequence?"}
    G -- yes --> EndRow["emit end_sequence row<br/>reset registers"]
    G -- no --> Mutate
    E --> Packed["advance address and line<br/>emit row"]
    Packed --> Row
```

### Standard Opcodes

Supported standard opcodes:

| Opcode | Effect |
| --- | --- |
| `DW_LNS_copy` | Emits the current registers as a row. |
| `DW_LNS_advance_pc` | Adds a ULEB128 delta to `address`. |
| `DW_LNS_advance_line` | Adds an SLEB128 delta to `line`. |
| `DW_LNS_set_file` | Sets the 1-based file index. |
| `DW_LNS_set_column` | Sets the source column. |
| `DW_LNS_negate_stmt` | Toggles `is_stmt`. |
| `DW_LNS_set_basic_block` | Marks the next row as a basic-block start. |
| `DW_LNS_const_add_pc` | Adds the fixed DWARF const-add address delta. |
| `DW_LNS_fixed_advance_pc` | Adds a 16-bit address delta. |
| `DW_LNS_set_prologue_end` | Marks the next row as a prologue end. |
| `DW_LNS_set_epilogue_begin` | Marks the next row as an epilogue begin. |
| `DW_LNS_set_isa` | Currently ignored. |

After an emitted normal row, these per-row flags are cleared:

- `basic_block_start`
- `prologue_end`
- `epilogue_begin`
- `discriminator`

### Extended Opcodes

Extended opcodes start with byte `0`, followed by a ULEB128 payload length and an
extended opcode byte.

| Opcode | Effect |
| --- | --- |
| `DW_LNE_end_sequence` | Emits a row with `end_sequence = true`, then resets the registers. |
| `DW_LNE_set_address` | Reads a 64-bit address and stores it as `file_addr`. |
| `DW_LNE_define_file` | Parses and appends a new file entry to `file_names_`. |
| `DW_LNE_set_discriminator` | Sets the discriminator for the next emitted row. |

### Special Opcodes

Special opcodes combine address and line movement into one byte and then emit a
row:

```text
adjusted_opcode = opcode - opcode_base
address += adjusted_opcode / line_range
line += line_base + (adjusted_opcode % line_range)
emit row
```

Because the parser requires minimum instruction length and maximum operations
per instruction to both be `1`, the current formula can stay simple.

## 8. Address-To-Line Lookup

`line_table::get_entry_by_address(file_addr address)` scans the emitted rows and
returns the row whose address range contains the requested address.

```mermaid
flowchart TD
    A["prev = begin()"] --> B{"empty table?"}
    B -- yes --> Z["return end"]
    B -- no --> C["it = next row"]
    C --> D{"it == end?"}
    D -- yes --> Z
    D -- no --> E{"prev.address <= address<br/>and it.address > address<br/>and !prev.end_sequence?"}
    E -- yes --> F["return prev"]
    E -- no --> G["prev = it<br/>++it"]
    G --> D
```

Conceptually:

```text
row N describes addresses [row N address, row N+1 address)
```

An `end_sequence` row terminates a contiguous address sequence and is not a
source row for lookup.

`dwarf::line_entry_at_address(file_addr)` first finds the compile unit whose
root DIE contains the address, then delegates to that compile unit's line table:

```mermaid
flowchart LR
    A["file_addr"] --> B["compile_unit_containing_address"]
    B --> C["cu->lines()"]
    C --> D["get_entry_by_address"]
    D --> E["line_table::iterator"]
```

## 9. Line-To-Address Lookup

`line_table::get_entries_by_line(path, line)` scans every emitted row and keeps
rows whose `line` matches and whose file path matches.

```mermaid
flowchart TD
    A["for row in line table"] --> B{"row.line == requested line?"}
    B -- no --> A
    B -- yes --> C{"path absolute?"}
    C -- yes --> D{"entry_path == path?"}
    C -- no --> E{"path_ends_in(entry_path, path)?"}
    D -- yes --> F["push iterator"]
    E -- yes --> F
    D -- no --> A
    E -- no --> A
    F --> A
```

This can return multiple rows for one source line. That is expected: one source
line may compile to several address ranges, especially when a statement contains
multiple operations or inline calls.

## 10. DIE Source Locations

`die::location()` is related to line tables but is not the same as address-row
lookup.

```text
die::location()
  -> die::file()
  -> die::line()
```

For non-inlined DIEs:

- file index comes from `DW_AT_decl_file`;
- line comes from `DW_AT_decl_line`.

For `DW_TAG_inlined_subroutine` DIEs:

- file index comes from `DW_AT_call_file`;
- line comes from `DW_AT_call_line`.

The file index is a 1-based index into `cu->lines().file_names()`.

## 11. Test Coverage Today

The line-table test builds `test/targets/hello_gsdb.cpp` with debug info and
checks direct line-table iteration.

Current source fixture:

```cpp
// 0x555555555147
#include <cstdio>

int main() { std::puts("Hello, gsdb!"); }
```

Current expected decoded line table in `test/tests.cpp`:

```text
hello_gsdb.cpp  4  0x1139
hello_gsdb.cpp  4  0x113d
hello_gsdb.cpp  4  0x114c
hello_gsdb.cpp  4  0x1151
hello_gsdb.cpp  -  0x1153
```

The test checks:

- one compile unit exists;
- the first row is line `4`;
- the first row's filename is `hello_gsdb.cpp`;
- subsequent normal rows are also line `4`;
- then an `end_sequence` row appears;
- one more increment reaches `end()`.

This test validates the iterator over the current compiler output. It is not a
portable semantic guarantee that a particular source line will always emit the
same number of rows.

## 12. Example Execution

For the current `hello_gsdb` target, `readelf --debug-dump=rawline` shows a
program shaped like this:

```text
set column 12
set address 0x1139
special opcode -> address 0x1139, line 4, emit
set column 23
special opcode -> address 0x113d, line 4, emit
set column 41
set discriminator 1
special opcode -> address 0x114c, line 4, emit
negate/set is_stmt false
special opcode -> address 0x1151, line 4, emit
advance pc to 0x1153
end_sequence -> emit terminating row
```

Timeline:

```mermaid
flowchart LR
    A["0x1139<br/>line 4"] --> B["0x113d<br/>line 4"]
    B --> C["0x114c<br/>line 4<br/>discriminator 1"]
    C --> D["0x1151<br/>line 4<br/>!is_stmt"]
    D --> E["0x1153<br/>end_sequence"]
```

## 13. Design Strengths

- The parser keeps the line-table matrix lazy, so it avoids storing every row.
- Paths are normalized to absolute paths when file entries are parsed.
- Relative path lookup is practical for user-facing commands such as
  `hello_gsdb.cpp:4` or `targets/hello_gsdb.cpp:4`.
- Address rows use `file_addr`, which keeps ELF ownership attached to the
  address and avoids confusing runtime virtual addresses with file addresses.
- The iterator API makes line-table scans simple for address and source-line
  lookup helpers.

## 14. Sharp Edges And Risks

These are implementation details worth keeping in mind before extending source
breakpoints or stepping.

### `die::contains()` Used Assignment Instead Of Comparison (fixed)

The predicate in `die::contains()` used assignment:

```cpp
return spec.attr = attribute;
```

Because the lambda received `spec` by value, this did not mutate the stored
abbreviation table, but it meant `contains(nonzero_attribute)` returned true for
the first attribute in any non-empty DIE — affecting line-table setup,
compile-unit address checks, and function indexing.

It now compares (`src/dwarf.cpp:954`):

```cpp
bool gsdb::die::contains(std::uint64_t attribute) const {
    auto& specs = abbrev_->attr_specs;
    return std::find_if(std::begin(specs), std::end(specs), [=](auto spec) {
               return spec.attr == attribute;
           }) != std::end(specs);
}
```

### `DW_LNS_set_isa` Does Not Consume Its Operand

The standard opcode length table says `DW_LNS_set_isa` has one operand. The
switch currently just `break`s. If a compiler emits this opcode, the cursor will
not advance past its ULEB128 operand and the rest of the line program will be
decoded incorrectly.

### Dynamic File Entries Can Mutate During Iteration

`DW_LNE_define_file` appends to `line_table::file_names_` during iteration. That
is why `file_names_` is `mutable`.

Consequences:

- repeated full iterations could append the same dynamically-defined files more
  than once;
- appending can reallocate the vector and invalidate `file_entry` pointers held
  by already-returned iterators.

This is probably harmless for current test binaries, but it matters if source
breakpoint code stores iterators long-term.

### `get_entries_by_line()` Includes End-Sequence Rows

The line-to-address scan filters by line and path, but not by
`!it->end_sequence`. An end-sequence row can retain the previous line/file state,
so a source-line lookup may include a terminating marker as if it were a real
address row.

For breakpoint placement, those rows should likely be excluded.

### Exact Row Counts Are Compiler-Sensitive

Line tables are compiler output. The current `hello_gsdb.cpp` line table emits
several rows for line 4 and no row for the blank line 3. Another compiler or
version may emit different rows while still being correct DWARF.

Tests that assert an exact row sequence are useful parser smoke tests, but they
can be brittle across toolchains.

### `line_table_` Can Be Null

`parse_line_table()` returns `nullptr` when a compile unit has no
`DW_AT_stmt_list`. `compile_unit::lines()` dereferences `line_table_` without a
guard. Current usage assumes debug line info exists.

### Header Support Is Narrow By Design

The implementation intentionally accepts only a narrow DWARF v4 shape:

- no DWARF64 line-table lengths;
- no DWARF v5 line-table layout;
- 64-bit addresses for `DW_LNE_set_address`;
- minimum instruction length `1`;
- maximum operations per instruction `1`;
- expected standard opcode lengths fixed to the v4 default table.

That is fine for the current test targets, but it is not a general-purpose
DWARF line parser yet.

## 15. Practical Extension Points

For source breakpoints:

```mermaid
flowchart TD
    A["user: break file.cpp:line"] --> B["choose compile units"]
    B --> C["cu->lines().get_entries_by_line"]
    C --> D["drop end_sequence rows"]
    D --> E["choose statement rows / all rows"]
    E --> F["convert file_addr to virt_addr"]
    F --> G["create breakpoint_site"]
```

For source-level stepping:

```mermaid
flowchart TD
    A["current PC virt_addr"] --> B["to file_addr"]
    B --> C["dwarf.line_entry_at_address"]
    C --> D["current source location"]
    D --> E["single-step / run until line changes"]
    E --> F["repeat lookup"]
```

Before building on this heavily, the likely cleanup order is:

1. ~~Fix `die::contains()`.~~ Done — see above.
2. Make `DW_LNS_set_isa` consume its operand (`src/dwarf.cpp:1415` still just
   `break`s).
3. Filter `end_sequence` in source-line lookup.
4. Decide whether `line_table` should materialize rows or keep lazy iteration.
5. If keeping lazy iteration, avoid long-lived `file_entry` pointers that can be
   invalidated by `DW_LNE_define_file`.
6. Make tests assert semantic properties, with one narrowly-scoped fixture test
   for exact opcode/row behavior if needed.
