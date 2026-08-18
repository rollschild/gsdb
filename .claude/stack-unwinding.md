# Stack Unwinding in gsdb — `gsdb::stack::unwind()`

Deep dive into `src/stack.cpp:57-106`, the routine that rebuilds the debugger's
view of the call stack every time the inferior halts.

All assembly in this document is **AT&T syntax** (`mnemonic src, dst`).

---

## 1. What problem is being solved?

When the inferior stops, the debugger holds exactly one thing: the hardware
register set of the *innermost* frame (`%rip`, `%rsp`, `%rbp`, callee-saved
regs). Everything else — "who called me, and with what registers?" — has to be
*reconstructed*.

Two independent reconstructions are interleaved in `unwind()`:

| Reconstruction | Source of truth | Produces |
| --- | --- | --- |
| **Physical unwinding** — walk from one machine frame to its caller | `.eh_frame` / `.eh_frame_hdr` (Call Frame Information) | a new `registers` value for the caller |
| **Logical un-inlining** — expand one machine frame into the chain of inlined functions the compiler folded into it | `.debug_info` (`DW_TAG_inlined_subroutine`) | extra `stack_frame`s that share one `registers` value |

`frames_` ends up holding the *logical* stack: innermost first, outermost last.

```
frames_[0]  innermost (possibly an inlined body)
frames_[1]
   ...
frames_[N]  outermost frame still inside the main ELF object
```

---

## 2. The loop's carried state

Four locals are threaded through the `while` loop. Getting these straight makes
the rest obvious.

| Local | Meaning | Updated at |
| --- | --- | --- |
| `regs` | Register set **of the frame currently being built**. Starts as a *copy* of the live hardware registers (`get_registers()` returns a reference; `auto` copies it). | line 100 |
| `virt_pc` | Runtime PC used for the loop's termination test. After the first iteration it is `return_address - 1`. | line 101 |
| `file_pc` | `virt_pc` translated into a file address (load bias removed) — the key for every DWARF lookup. | line 103 |
| `elf` | The ELF object `file_pc` belongs to; `nullptr` once the PC leaves the main executable. | line 104 |

`registers` is a value type here, so each `stack_frame` owns an independent
snapshot. That is what makes `stack::up()` / `down()` + `regs()` work: selecting
frame *N* means reading frame *N*'s saved register copy.

---

## 3. Control flow

```
                 unwind()
                    │
    ┌───────────────▼────────────────┐
    │ reset_inline_height()          │  how many inlined frames is the PC
    │ current_frame_ = inline_height_│  sitting exactly at the START of?
    └───────────────┬────────────────┘
                    │
    ┌───────────────▼────────────────┐
    │ virt_pc = process PC           │
    │ file_pc = PC as file address   │
    │ regs    = copy of live regs    │
    │ frames_.clear()                │
    └───────────────┬────────────────┘
                    │
             elf = file_pc.elf_file()
                    │
            ┌───────▼────────┐   nullptr
            │  elf valid?    ├──────────────► return  (PC not in any section)
            └───────┬────────┘
                    │ yes
    ╔═══════════════▼═══════════════════════════════════════════╗
    ║  while (virt_pc != 0 && elf == &target_->get_elf())        ║
    ╠════════════════════════════════════════════════════════════╣
    ║                                                            ║
    ║   inline_stack = dwarf.inline_stack_at_address(file_pc)    ║
    ║        │                                                   ║
    ║        ├── empty ──────────────────────► return (no DWARF) ║
    ║        │                                                   ║
    ║   ┌────▼─────────────┐                                     ║
    ║   │ size() > 1 ?     │                                     ║
    ║   └─┬──────────────┬─┘                                     ║
    ║  yes│              │no                                     ║
    ║     ▼              ▼                                       ║
    ║  create_base_frame(..., inlined=true)                      ║
    ║  create_inline_stack_frames(...)   create_base_frame(...,  ║
    ║     │                                        inlined=false)║
    ║     └──────────────┬─────────────────────────┘             ║
    ║                    ▼                                       ║
    ║   regs = dwarf.cfi().unwind(proc, file_pc,                 ║
    ║                             frames_.back().regs)  ◄── CFI  ║
    ║   virt_pc = virt_addr{ regs.rip - 1 }                      ║
    ║   file_pc = virt_pc.to_file_addr(main elf)                 ║
    ║   elf     = file_pc.elf_file()                             ║
    ║                    │                                       ║
    ╚════════════════════╪═══════════════════════════════════════╝
                         │ loop / exit
                         ▼
                       done
```

One iteration = **one physical frame** (which may emit several logical frames).

---

## 4. Physical vs. logical frames

Suppose the source is:

```cpp
static inline int baz(int x) { return x * 2; }      // inlined
static inline int bar(int x) { return baz(x) + 1; } // inlined
int foo(int x)  { return bar(x); }                  // real function
int main()      { return foo(21); }                 // real function
```

The machine stack has **two** frames. The debugger should show **four**.

```
   MACHINE STACK (2 physical frames)        LOGICAL STACK (what gsdb shows)
   ───────────────────────────────────      ────────────────────────────────
                                            frames_[0]  baz   (inlined=true)
   ┌──────────────────────────┐             frames_[1]  bar   (inlined=true)
   │ foo's frame              │  ◄── PC ──► frames_[2]  foo   (inlined=false)
   │  (baz and bar folded in) │
   ├──────────────────────────┤
   │ main's frame             │  ◄────────► frames_[3]  main  (inlined=false)
   ├──────────────────────────┤
   │ __libc_start_call_main   │             (different ELF → loop stops)
   └──────────────────────────┘
```

Iteration 1 handles `foo`'s physical frame and emits `frames_[0..2]`.
Iteration 2 handles `main`'s physical frame and emits `frames_[3]`.

### 4.1 `inline_stack_at_address()` ordering

`dwarf::inline_stack_at_address()` (`src/dwarf.cpp:1525`) starts from the
concrete `DW_TAG_subprogram` containing the address, then repeatedly descends
into whichever `DW_TAG_inlined_subroutine` child also contains the address:

```
inline_stack = [ foo , bar , baz ]
                 ^            ^
                 |            └── back()  = innermost = what is EXECUTING
                 └─────────────── front() = the real, physical function
```

So the vector is **outermost → innermost**, the opposite order of `frames_`.
That is why both `create_base_frame` and `create_inline_stack_frames` walk it
with reverse iterators.

### 4.2 `create_base_frame` (`src/stack.cpp:111`)

Emits the innermost logical frame. Note that the function names none of the
`stack_frame` fields: the `push_back` at `src/stack.cpp:123-125` is a **braced
aggregate initializer with no designated initializers**, so each value lands in
a member purely by position. The field names below come from the `stack_frame`
definition at `include/libgsdb/stack.hpp:15-21`:

```
  frames_.push_back({ regs , backtrace_pc            , inline_stack.back() , inlined , source_location{...} });
                       │      │                        │                     │         │
  struct stack_frame { │      │                        │                     │         │
      registers        regs;  │                        │                     │         │
      virt_addr        backtrace_report_address;  ◄────┘ (local: backtrace_pc)│         │
      die              func_die;              ◄─────────────────────────────┘          │
      bool             inlined = false;    ◄────────────────────────────────────────┘  │
      source_location  location;        ◄───────────────────────────────────────────────┘
  };
```

Field by field:

- `regs` = the frame's register snapshot.
- **`backtrace_report_address`** — held by the local `backtrace_pc`
  (`src/stack.cpp:114`). Its value is the start address of the *line table
  entry* covering `pc`, not `pc` itself. For a caller frame `pc` is already
  `ra - 1` (§6), so this snaps the report back to the beginning of the source
  line containing the `call`.
- `func_die = inline_stack.back()` — the deepest inlined body, not the physical
  function.
- `inlined = (inline_stack.size() > 1)`.
- `location` = the file/line of that same line table entry.

#### 4.2.1 The line-table lookup (`src/stack.cpp:114-121`)

```cpp
auto backtrace_pc = pc.to_virt_addr();                                  // 114
// Find the start of the call instruction, ...                         // 115-117
auto line_entry = pc.elf_file()->get_dwarf().line_entry_at_address(pc); // 118
if (line_entry != line_table::iterator{}) {                             // 119
    backtrace_pc = line_entry->address.to_virt_addr();                  // 120
}                                                                       // 121
```

Four lines deciding **which address this frame reports**. Line 114 seeds it with
the raw `pc`; 118–121 try to replace that with something better and fall back to
the raw value if they can't.

**The problem being solved.** For every frame except the innermost, the `pc`
handed to `create_base_frame` is `ra - 1` — the deliberate bias from §6. That
address is *inside* the `call` instruction, not at its start:

```
 source line 7:   int r = foo(21);
 source line 8:   return r;

 .debug_line rows                machine code
 ┌──────────────────────┐
 │ addr=0x1165  line=7  │──►  0x1165   movl   $21, %edi
 └──────────────────────┘     0x116a   callq  foo          ◄─ 5 bytes
                              0x116f   movl   %eax, -4(%rbp)
 ┌──────────────────────┐
 │ addr=0x1172  line=8  │──►  0x1172   movl   -4(%rbp), %eax
 └──────────────────────┘

   return address stored by the call   ra      = 0x116f
   pc handed to create_base_frame      ra - 1  = 0x116e   ◄─ last byte of `callq`
   get_entry_by_address(0x116e)        → the row covering [0x1165, 0x1172)
   line_entry->address                 = 0x1165           ◄─ what gets reported
```

`0x116e` is useless to show a user: it is not an instruction boundary, so it
can't be disassembled from, can't take a breakpoint, and reads as a corrupt
address in a backtrace. Snapping to `line_entry->address` gets back to a real
instruction, and specifically to the first instruction the compiler generated
for the row covering the call.

**Why the line table rather than something else.** Three alternatives, and why
each loses:

| Approach | Problem |
| --- | --- |
| Report `pc` unchanged | Mid-instruction address for every caller frame |
| Report the function's `low_pc` | Every frame in a function collapses to one address — the call site, the only interesting part, is lost |
| Disassemble backward to find the `call` | x86-64 is variable-length, so backward decode is a heuristic, not a decision; it drags the disassembler into the stack layer, and still yields no file/line |

The real argument is the one visible at line 125: **the line entry has to be
fetched anyway.** `source_location{line_entry->file_entry, line_entry->line}` is
the frame's file/line, and there is no way to get it except from the line table.
Once you hold that row, its `address` field is free — and taking both from the
same row *guarantees the reported address and the reported line agree*. Compute
the address by disassembly and the line from the table and they can drift apart;
take both from one row and they cannot.

**How the lookup works.** `dwarf::line_entry_at_address`
(`include/libgsdb/dwarf.hpp:370-374`) finds the CU containing the address, then
calls `line_table::get_entry_by_address` (`src/dwarf.cpp:1467-1483`), which
walks the rows looking for the pair where `prev->address <= address <
it->address` and `prev` is not an `end_sequence` marker. So it returns the row
whose address range *covers* the pc — a containment query, not an exact match,
which is exactly right for a pc pointing into the middle of a row's code.

**The `!= line_table::iterator{}` guard.** The lookup can fail two ways: no CU
contains the address (`dwarf.hpp:372` returns `{}`), or the walk falls off the
end (`src/dwarf.cpp:1482` returns `end()`). `line_table::end()` is literally
`return {}` (`src/dwarf.cpp:1341`), and `operator==` compares only `pos_`
(`dwarf.hpp:309`), which value-initialization zeroes to `nullptr`. So
`line_entry != line_table::iterator{}` is a comparison against `end()` written
longhand — "did the lookup find a row?" On failure `backtrace_pc` keeps the raw
`pc` from line 114, which is degraded but not wrong.

Two caveats on this block:

- **The comment at 115–117 overstates what is computed.** It says "the start of
  the call instruction", but `line_entry->address` is the start of the *row*,
  which is `<=` the call's address. In the diagram above the row starts at
  `0x1165` (`movl $21, %edi`, setting up the argument), five bytes before the
  `callq` at `0x116a`. For a bare statement the two coincide; for anything with
  argument setup or a compound expression — `foo(bar(x), baz(y))` — the row
  starts well before the call being reported. "Start of the code for this source
  line" is the accurate description.
- **The guard does not protect line 125.** `line_entry` is dereferenced
  unconditionally when building `source_location`, outside the `if`. See §11
  item 1.

### 4.3 `create_inline_stack_frames` (`src/stack.cpp:128`)

Emits the remaining logical frames for the *same* physical frame, walking
outward from the second-innermost entry:

```
for it = rbegin()+1 .. rend():          // bar, then foo
    inlined_pc = prev(it)->low_pc()     // low_pc of the INNER function
    location   = prev(it)->location()   // DW_AT_call_file/call_line of the inner one
    inlined    = (next(it) != rend())   // false only for the concrete function
    push {regs, inlined_pc, *it, inlined, location}
```

`std::prev` on a reverse iterator moves *inward*, so each frame is described by
the call site of the function it inlined — exactly the "called from here"
semantics a backtrace needs. Note every frame in the group shares the **same
`regs` copy**: inlined functions have no frame of their own.

```
 inline_stack:   [ foo ][ bar ][ baz ]
                    ▲      ▲      ▲
 rend()─────────────┘      │      └──── rbegin()
                           │
 create_base_frame     ────┴──► pushes  baz   (from back())
 create_inline_stack_frames:
     it = &bar  → prev(it) = &baz → pushes bar  @ baz.low_pc, loc = baz's call site
     it = &foo  → prev(it) = &bar → pushes foo  @ bar.low_pc, loc = bar's call site
```

---

## 5. `inline_height_` — pretending we haven't stepped in yet

`reset_inline_height()` (`src/stack.cpp:26`) counts, from the innermost outward,
how many inlined frames have `low_pc() == pc` — i.e. the PC sits exactly on the
first instruction of an inlined body.

Conceptually: at the instant control reaches `baz`'s first inlined instruction,
`step`-ing should behave as if the user is still in `bar` and must `step` again
to "enter" `baz`. There is no `call`, so there is no natural stop; gsdb
simulates it with a counter.

```
 frames_ (all frames, innermost first)

   index 0   baz    ◄─┐
   index 1   bar      │  inline_height_ = 2  → hidden by frames()
   index 2   foo    ◄─┘
   index 3   main

   frames()  = span starting at frames_.data() + inline_height_
             = [ foo , main ]          ← what the user sees
   current_frame_ = inline_height_ = 2 ← the "current" frame is foo
```

`stack::simulate_inlined_step_in()` just does `--inline_height_`, which slides
the window one frame deeper — the user appears to step into `bar`, then `baz`,
without a single instruction being executed. `target::step_in()`
(`src/target.cpp:82`) takes that shortcut before touching `ptrace`.

`unwind()` sets `current_frame_ = inline_height_` (line 59) so the selected
frame is the first *visible* one.

---

## 6. Why `- 1` on the return address (line 102)

The unwound `%rip` is a **return address**: it points at the instruction *after*
the `call`. Using it directly for lookups is wrong in two ways.

```
   foo:
     ...
     0x1140:  callq  bar          ◄── the call site: what we want to report
     0x1145:  movl   %eax, -4(%rbp)   ◄── return address stored on the stack
     ...
     0x1160:  retq
   bar:                               ◄── if the call is the LAST instruction,
     0x1161:  ...                         ra may land in the NEXT function

   virt_pc = ra - 1 = 0x1144  →  falls inside the `callq` instruction
```

Consequences of the `-1`:

1. `function_containing_address` / the FDE range test resolve to the **caller**,
   not to whatever follows a noreturn or tail-position call.
2. `line_entry_at_address` yields the source line of the `call`, so backtraces
   say "line 12" (the call) rather than "line 13" (the statement after it).

Important: only the *lookup* PC is biased. `frames_.back().regs` still holds the
true, un-decremented `%rip`, so resuming or reading registers from that frame is
correct. `backtrace_report_address` gets the line-entry start (§4.2), which is
the address actually printed.

---

## 7. The CFI step: `dwarf.cfi().unwind(...)`

Line 99 is where one physical frame is popped. Implementation:
`gsdb::call_frame_information::unwind` (`src/dwarf.cpp:1625`).

```
 file_pc
    │
    ▼
 ┌──────────────────────────────────────────────────────────┐
 │ 1. eh_hdr_[pc]                                           │
 │    binary search the .eh_frame_hdr sorted table          │
 │    (initial_address, fde_ptr) pairs  → pointer to an FDE │
 └───────────────────────┬──────────────────────────────────┘
                         ▼
 ┌──────────────────────────────────────────────────────────┐
 │ 2. parse_fde(); assert                                   │
 │      initial_location <= pc < initial_location + range   │
 │    else  error "No unwind information at PC!"            │
 └───────────────────────┬──────────────────────────────────┘
                         ▼
 ┌──────────────────────────────────────────────────────────┐
 │ 3. run the CIE's *initial* instructions to completion    │
 │      → the default rules shared by every FDE of this CIE │
 │    snapshot them into ctx.cie_register_rules             │
 │      (this is what DW_CFA_restore restores TO)           │
 └───────────────────────┬──────────────────────────────────┘
                         ▼
 ┌──────────────────────────────────────────────────────────┐
 │ 4. run the FDE's instructions while                      │
 │      !cursor.finished() && ctx.location <= pc            │
 │    DW_CFA_advance_loc* bumps ctx.location; execution     │
 │    stops once the table has advanced past our PC         │
 └───────────────────────┬──────────────────────────────────┘
                         ▼
 ┌──────────────────────────────────────────────────────────┐
 │ 5. execute_unwind_rules(): apply the rule set to `regs`  │
 └──────────────────────────────────────────────────────────┘
```

### 7.1 The CFI virtual machine

The CFI byte stream is a program that builds one *row* of a conceptual table:
"at address L, register R can be recovered by rule X". `unwind_context`
(`src/dwarf.cpp:652`) is that row.

| Rule (`src/dwarf.cpp:636-650`) | Meaning | Applied as |
| --- | --- | --- |
| `undefined_rule` | value is lost in this frame | `regs.undefine(id)` |
| `same_rule` | callee preserved it — already correct | no-op |
| `offset_rule{n}` | saved in memory | `reg = *(uint64_t*)(CFA + n)` |
| `val_offset_rule{n}` | the *value* is `CFA + n` | `reg = CFA + n` |
| `register_rule{r}` | old value lives in register `r` | `reg = old_regs[r]` |
| `cfa_register_rule{r,n}` | how to compute the CFA itself | `CFA = old_regs[r] + n` |

`DW_CFA_remember_state` / `DW_CFA_restore_state` push/pop the whole row onto
`ctx.rule_stack` — compilers emit these around epilogues and cold paths.
`DW_CFA_*expression` opcodes currently `error::send("DWARF expressions not yet
implemented!")`.

### 7.2 CFA and the frame boundary

The **Canonical Frame Address** is the value `%rsp` had in the caller
immediately before the `call` pushed the return address. It is the one anchor
that stays fixed for the whole lifetime of a frame, even while `%rsp` moves.

```
 higher addresses
        ┌───────────────────────────┐
        │  caller's locals ...      │
        ├───────────────────────────┤  ◄── CFA of the current frame
        │  return address           │      == caller's %rsp before `call`
        ├───────────────────────────┤      typically at CFA-8  (r16 rule)
        │  saved %rbp               │      typically at CFA-16 (r6 rule)
        ├───────────────────────────┤  ◄── %rbp  (after `movq %rsp, %rbp`)
        │  locals / spills          │
        ├───────────────────────────┤
        │  outgoing args            │
        └───────────────────────────┘  ◄── %rsp
 lower addresses
```

Typical x86-64 CIE + prologue for a frame-pointer function:

```
   CIE initial:   DW_CFA_def_cfa r7 (%rsp) offset 8      ; CFA = rsp + 8
                  DW_CFA_offset  r16 (rip) at cfa-8      ; return addr at CFA-8

   pushq %rbp     DW_CFA_def_cfa_offset 16               ; CFA = rsp + 16
                  DW_CFA_offset r6 (%rbp) at cfa-16
   movq %rsp,%rbp DW_CFA_def_cfa_register r6             ; CFA = rbp + 16
```

`execute_unwind_rules` (`src/dwarf.cpp:809`) then does:

```cpp
cfa = old_regs[cfa_rule.reg] + cfa_rule.offset;
old_regs.set_cfa(virt_addr{cfa});          // annotate the frame we just left
unwound_regs = old_regs;                   // start from a copy
unwound_regs[rsp] = cfa;                   // caller's %rsp IS the CFA
for (reg, rule) : register_rules           //   ... plus each restore rule
    apply(rule);                           //   (rip comes from the r16 rule)
```

Note the key identity: **the caller's `%rsp` is exactly this frame's CFA**, and
the caller's `%rip` falls out of the `r16` (return address) rule. Everything the
next loop iteration needs is produced by these two.

DWARF register numbers used on x86-64:

| # | reg | # | reg | # | reg |
| --- | --- | --- | --- | --- | --- |
| 0 | `%rax` | 4 | `%rsi` | 8–15 | `%r8`–`%r15` |
| 1 | `%rdx` | 5 | `%rdi` | 16 | return address (`%rip`) |
| 2 | `%rcx` | 6 | `%rbp` | | |
| 3 | `%rbx` | 7 | `%rsp` | | |

---

## 8. Termination

| Condition | Line | When it fires |
| --- | --- | --- |
| `file_pc.elf_file() == nullptr` before the loop | 70 | PC not inside any section of the main ELF (JIT, vDSO, corrupted PC) |
| `elf != &target_->get_elf()` | 78 | The unwound return address left the main executable — almost always the libc frame that called `main` (`__libc_start_call_main`). This is the normal exit. |
| `virt_pc.addr() == 0` | 78 | Guards a zero PC. Note that after the first iteration `virt_pc` is `ra - 1`, so a zero return address wraps to `0xFFFF'FFFF'FFFF'FFFF` and is caught by the ELF test instead, not by this one. |
| `inline_stack.empty()` | 83–85 | No `DW_TAG_subprogram` covers the PC — a PLT stub, or a function compiled without debug info. Truncates the backtrace at that point. |

`virt_addr::to_file_addr()` is what makes the ELF test work: it calls
`get_section_containing_address()` first and returns a default-constructed
`file_addr` (null `elf_`) when the address is not in a section of the object.

---

## 9. Worked example

Target stopped inside inlined `baz`; `main` → `foo`(+`bar`+`baz`).

```
 ── entry ─────────────────────────────────────────────────────────────
 reset_inline_height() → 0        (PC is mid-body, not at an inline low_pc)
 current_frame_ = 0
 virt_pc = 0x5555'5555'1152       file_pc = 0x1152        elf = main ELF
 regs    = live hardware registers
 frames_ = []

 ── iteration 1 ───────────────────────────────────────────────────────
 inline_stack_at_address(0x1152) = [foo, bar, baz]   size 3 > 1
   create_base_frame(inlined=true)   → frames_[0] = baz  (regs = live)
   create_inline_stack_frames()      → frames_[1] = bar  (regs = live)
                                       frames_[2] = foo  (regs = live)
 cfi().unwind(pc=0x1152, frames_.back().regs):
   eh_hdr binary search           → FDE for foo
   CIE rules   : CFA = rsp+8, rip @ CFA-8
   FDE rules   : CFA = rbp+16,  rbp @ CFA-16       (past the prologue)
   CFA         = rbp + 16       = 0x7fff'ffff'e260
   new rsp     = CFA            = 0x7fff'ffff'e260
   new rip     = [CFA-8]        = 0x5555'5555'1171   ← return into main
   new rbp     = [CFA-16]       = 0x7fff'ffff'e280
 virt_pc = 0x5555'5555'1170   (ra - 1, lands inside `callq foo`)
 file_pc = 0x1170             elf = main ELF   → keep going

 ── iteration 2 ───────────────────────────────────────────────────────
 inline_stack_at_address(0x1170) = [main]            size 1
   create_base_frame(inlined=false)  → frames_[3] = main
        backtrace_report_address = start of the line-table entry for
        the `foo(21)` call, i.e. the source line of the call, not 0x1171
 cfi().unwind(...) → rip = 0x7fff'f7d... inside libc
 virt_pc = that - 1
 file_pc = virt_pc.to_file_addr(main elf)  → not in any section
 elf     = nullptr  ≠ &get_elf()           → loop exits

 ── result ────────────────────────────────────────────────────────────
 frames_ = [ baz(inl), bar(inl), foo, main ]
 inline_height_ = 0  → frames() shows all four
 current_frame_ = 0  → current frame is baz
```

Backtrace output shape:

```
 #0  baz(int)   at demo.cpp:3     [inlined]
 #1  bar(int)   at demo.cpp:4     [inlined]
 #2  foo(int)   at demo.cpp:7
 #3  main       at demo.cpp:10
```

---

## 10. Cost

Per halt:

- one `.eh_frame_hdr` binary search per physical frame — `O(log F)` where `F` is
  the FDE count;
- CIE + FDE instruction replay per physical frame — linear in the FDE's byte
  length, re-executed from scratch every time (no row caching);
- one `read_memory` (`PTRACE_PEEKDATA`-class) call **per `offset_rule`** in
  `execute_unwind_rules` — typically 2–4 per frame;
- DIE-tree descent per frame for the inline stack, plus the lazily built
  `function_index_` on first use.

Called from `target::notify_stop()` (`src/target.cpp:76`), i.e. after *every*
stop — including every single instruction of a source-level `step`. This is the
main reason `step_in`/`step_over` loops feel slow on large binaries.

---

## 11. Notes and caveats

Observations from reading the current implementation. Nothing here has been
changed — flagged for review only.

1. **`create_base_frame` dereferences `line_entry` outside its own guard**
   (`src/stack.cpp:119-125`). The `if (line_entry != line_table::iterator{})`
   test protects the `backtrace_pc` assignment, but `line_entry->file_entry` and
   `line_entry->line` on line 125 run unconditionally. A default-constructed
   iterator's `operator->` returns `&current_` (a default `entry`), so this is
   not UB, but the frame silently gets `source_location{nullptr, 1}` — a null
   file pointer that any consumer must handle.

2. **`ctx.register_rules.emplace(...)` never overwrites.**
   `std::unordered_map::emplace` is a no-op when the key already exists, yet
   every rule-setting opcode in `execute_cfi_instruction`
   (`src/dwarf.cpp:692-790`) uses it. Per the DWARF spec a later row must
   *replace* an earlier rule for the same register. As written, `DW_CFA_restore`
   / `DW_CFA_restore_extended` cannot undo an earlier `DW_CFA_offset`, and a
   register saved to a new slot mid-function keeps the stale rule. Functions
   with multiple epilogues or shrink-wrapped saves are the likely failure mode.
   `insert_or_assign` (or `operator[]`) would be the spec-conforming call.
   `cfa_rule` is a plain member and is assigned directly, so the CFA itself is
   unaffected.

3. **Undefined return address throws rather than terminating the loop.** If CFI
   marks r16 `DW_CFA_undefined` (the convention at `_start`),
   `regs.undefine(rip)` runs and line 102's `read_by_id_as` hits the
   `"Register is undefined"` throw in `registers::read` (`src/registers.cpp:47`).
   Dynamically linked binaries never reach this because the loop exits on the
   libc frame first; a statically linked target unwound to `_start` would.

4. **Empty `frames_` is reachable.** If the very first
   `inline_stack_at_address` comes back empty (PC in a PLT stub, or in a
   no-debug-info function of the main ELF), `unwind()` returns with `frames_`
   empty while `current_frame_` may be non-zero. `regs()`,
   `get_pc()`, and `current_frame()` all index `frames_` unguarded, so callers
   must check `has_frames()` first.

5. **No shared-library unwinding.** The loop is explicitly single-object
   (`elf == &target_->get_elf()`). Backtraces stop at the executable boundary,
   so a crash inside libc shows nothing, and a callback invoked from a library
   loses everything above it. Supporting this needs a per-module ELF/DWARF
   registry keyed by the runtime address ranges from `/proc/<pid>/maps` or the
   dynamic linker's rendezvous structure.

6. **No unwind row caching.** CIE + FDE instructions are re-executed on every
   halt for every frame. A cache keyed by `(fde_offset, pc)` — or even just
   memoizing the parsed CIE rule set, which `cie_map_` already partly does for
   the CIE header — would cut the per-step cost.

---

## 12. Related source

| Concern | Location |
| --- | --- |
| `unwind()`, frame construction | `src/stack.cpp:57-138` |
| `stack` / `stack_frame` definitions | `include/libgsdb/stack.hpp` |
| CFI driver | `src/dwarf.cpp:1625` (`call_frame_information::unwind`) |
| `.eh_frame_hdr` binary search | `src/dwarf.cpp:1575` (`eh_hdr::operator[]`) |
| CFI opcode interpreter | `src/dwarf.cpp:666` (`execute_cfi_instruction`) |
| Rule application | `src/dwarf.cpp:809` (`execute_unwind_rules`) |
| Inline stack from DIEs | `src/dwarf.cpp:1525` (`inline_stack_at_address`) |
| Stop hook that drives it all | `src/target.cpp:76` (`target::notify_stop`) |
| Pointer-encoding background | `.claude/got-plt-and-eh-frame-pointer-encodings.md` |
