# `%rip`, `%rsp`, `%rbp` — The Three Pointers

The three registers a debugger cares about most. Each points into a different
place, and together they answer "where am I, and how did I get here?"

```
  %rip  ->  which instruction        "where am I"
  %rsp  ->  top of the stack         "where does scratch space start"
  %rbp  ->  base of current frame    "where do my locals/args live"
```

## 1. They Point Into Different Memory

```
   high addresses
   +----------------------------------+
   |   stack                          |
   |       [caller frame]             |
   |       [my frame]        <- %rbp  |   points at the frame base
   |       [scratch]         <- %rsp  |   points at the top of stack
   |          |  grows DOWN           |
   |          v                       |
   +----------------------------------+
   |            (unmapped gap)        |
   +----------------------------------+
   |   heap    ^ grows up             |
   +----------------------------------+
   |   .data / .bss   (globals)       |
   +----------------------------------+
   |   .text   (machine code) <- %rip |   points at the next instruction
   +----------------------------------+
   low addresses
```

`%rip` lives in the code segment. `%rsp` and `%rbp` both live in the stack,
pointing at two ends of the *same* frame.

## 2. Which Way Is Up: "Top" Means Lowest Address

The stack grows **downward**, toward lower addresses. "Top of the stack" is
meant in the *abstract data structure* sense — the most recently pushed item —
which on x86-64 is the numerically **lowest** occupied address.

```
   high addresses
      0x7ff8  | oldest data      |   <- "bottom" of the stack (pushed first)
      0x7ff0  |                  |
      0x7fe8  |                  |
      0x7fe0  | newest data      |   <- %rsp   "top" of the stack
              |                  |
      0x7fd8  | (free space)     |        push grows into here
   low addresses
```

```
   push  ->  %rsp moves DOWN (toward 0)
   pop   ->  %rsp moves UP
```

Because addresses grow up while the stack grows down, an **older frame sits at
higher addresses** than a newer one. That is why the caller's data is "above"
yours, and why `[%rbp + 8]` — a *positive* offset — reaches the return address
the caller pushed.

### `%rsp` points AT valid data, not past it

x86 uses a **full descending** stack: `[%rsp]` is the last item pushed and is
readable right now.

```
   push %rax   ->   rsp -= 8 ; [rsp] = rax        store AFTER decrement
   pop  %rax   ->   rax = [rsp] ; rsp += 8        load BEFORE increment
```

Some architectures instead use an *empty descending* stack, where `%sp` points
at the next free slot. That is an 8-byte difference in every hand-computed
address, so it is worth being explicit about which convention applies.

### Below `%rsp` is not garbage: the red zone

The SysV ABI reserves 128 bytes at `[%rsp - 128, %rsp)` as a **red zone** that
leaf functions may use as scratch *without* adjusting `%rsp` at all. Signal
handlers and the kernel are required not to clobber it.

```
      0x7fe0  | live data        |   <- %rsp
              |                  |
              |   RED ZONE       |   128 bytes, usable by leaf functions
              |   (still live!)  |   without moving %rsp
      0x7f60  +------------------+   <- %rsp - 128
              |  genuinely free  |
```

This matters for a debugger: if you inject a call into the inferior, skip past
the red zone (`rsp -= 128`) before pushing anything, or you will silently
corrupt the current function's locals.

## 3. AT&T Syntax: `src, dst`

```
        mov    %rsp,  %rbp
         |       |      |
         |       |      +--- DESTINATION  (written)
         |       +---------- SOURCE       (read)
         +------------------ opcode

    reads as:  rbp <- rsp
    Intel would write the identical instruction as:  mov rbp, rsp
```

| | AT&T | Intel |
|---|---|---|
| operand order | `src, dst` | `dst, src` |
| register prefix | `%rax` | `rax` |
| immediate prefix | `$0x10` | `0x10` |
| size suffix | `movq`, `movl` | `mov qword ptr` |
| memory operand | `-0x8(%rbp)` | `[rbp-8]` |

No suffix on `mov %rsp,%rbp` because both operands are 64-bit registers, so the
width is unambiguous.

## 4. Who Moves What

```
   instruction        %rip        %rsp        %rbp
   ------------------------------------------------------
   mov, add, ...       +len         -           -
   jmp / jcc          target        -           -
   push %reg           +len        -8           -
   pop  %reg           +len        +8           -
   sub $N,%rsp         +len        -N           -
   call target        target       -8           -      <- couples rip & rsp
   ret                [%rsp]       +8           -      <- couples rip & rsp
   mov %rsp,%rbp       +len         -         = %rsp   <- couples rsp & rbp
   leave               +len       = %rbp+8   [%rbp]
```

Only three instruction families touch more than one of them. **`call`/`ret` are
where `%rip` and `%rsp` meet: the stack is where old `%rip` values are stored.**

`%rip` is also special in that you cannot write it directly — no
`mov $0x1234,%rip`. It changes only through control flow. A debugger writes it
out-of-band through `ptrace`.

## 5. `call` and `ret`, Step by Step

`call foo` is two operations fused into one instruction:

```
        push  <address of the next instruction>     # rsp -= 8; [rsp] = ret
        jmp   foo                                   # rip = foo
```

```
   BEFORE  `call foo`                      AFTER  `call foo`

   code                                    code
     0x1000: call foo    <- %rip             0x1000: call foo
     0x1005: mov  ...                        0x1005: mov  ...
     0x2000: push %rbp   (foo)               0x2000: push %rbp   <- %rip

   stack                                   stack
     0x7ff8 | caller local |                 0x7ff8 | caller local |
     0x7ff0 | caller local | <- %rsp         0x7ff0 | caller local |
     0x7fe8 |    unused    |                 0x7fe8 |    0x1005    | <- %rsp
     0x7fe0 |    unused    |                 0x7fe0 |    unused    |
                                                        ^
                                                        return address
                                                        = the saved %rip
```

`ret` reverses it exactly: `rip = [rsp]; rsp += 8`.

```
   BEFORE `ret`                            AFTER `ret`

     0x7fe8 |    0x1005    | <- %rsp         0x7ff0 | caller local | <- %rsp
                                             %rip = 0x1005
```

## 6. The Prologue, Frame by Frame

The sequence you see at the top of nearly every unoptimized function:

```
   foo:
     0x2000:  push %rbp
     0x2001:  mov  %rsp,%rbp
     0x2004:  sub  $0x10,%rsp
```

```
  (a) on entry to foo                (b) after  push %rbp
      %rsp=0x7fe8  %rbp=CALLER           %rsp=0x7fe0  %rbp=CALLER

      0x7ff0 | caller data  |            0x7ff0 | caller data  |
      0x7fe8 | ret 0x1005   | <- rsp     0x7fe8 | ret 0x1005   |
      0x7fe0 |   unused     |            0x7fe0 | saved rbp    | <- rsp
      0x7fd8 |   unused     |            0x7fd8 |   unused     |


  (c) after  mov %rsp,%rbp           (d) after  sub $0x10,%rsp
      %rsp=0x7fe0  %rbp=0x7fe0           %rsp=0x7fd0  %rbp=0x7fe0

      0x7ff0 | caller data  |            0x7ff0 | caller data  |
      0x7fe8 | ret 0x1005   |            0x7fe8 | ret 0x1005   |
      0x7fe0 | saved rbp    | <- rsp     0x7fe0 | saved rbp    | <- rbp
                              <- rbp     0x7fd8 | local a      |
      0x7fd8 |   unused     |            0x7fd0 | local b      | <- rsp
```

Step (c) is the `mov %rsp,%rbp` — it *pins* `%rbp` to the frame base so that
`%rsp` is then free to move around underneath it.

## 7. The Resulting Frame Map

```
                            higher addresses
      +----------------------+
      |   caller's frame     |
      +======================+ <-- CFA (caller's %rsp at the call site)
      |   return address     |   [%rbp + 8]    the saved %rip
      +----------------------+
      |   saved %rbp         |   [%rbp + 0]    <- %rbp   the caller's %rbp
      +----------------------+
      |   local a            |   [%rbp - 8]
      |   local b            |   [%rbp - 16]
      +----------------------+
      |   outgoing args /    |
      |   scratch            |   <- %rsp
      +----------------------+
                            lower addresses
```

Two rules follow, and they are the whole reason the frame pointer exists:

```
   locals & spills   ->  NEGATIVE offsets from %rbp     -0x8(%rbp)
   return addr/args  ->  POSITIVE offsets from %rbp     0x10(%rbp)
```

Offsets from `%rbp` are *constant* for the whole function, while offsets from
`%rsp` shift every time something is pushed.

## 8. The Epilogue

```
     leave        # equivalent to:  mov %rbp,%rsp   (discard locals)
                  #                 pop %rbp        (restore caller's rbp)
     ret          # rip = [rsp]; rsp += 8
```

```
   before leave         after mov %rbp,%rsp    after pop %rbp     after ret
   rsp=0x7fd0           rsp=0x7fe0             rsp=0x7fe8         rsp=0x7ff0
   rbp=0x7fe0           rbp=0x7fe0             rbp=CALLER         rip=0x1005
```

The stack is back exactly where it was before the `call`.

## 9. The 16-Byte Alignment Invariant

The SysV ABI requires `%rsp` to be 16-byte aligned *at the moment of the `call`*.

```
   caller, at the call instruction ......  %rsp % 16 == 0
   call pushes 8 bytes ..................  %rsp % 16 == 8   <- callee entry
   push %rbp pushes 8 bytes .............  %rsp % 16 == 0   <- aligned again
```

Break this and SSE instructions such as `movaps` in libc fault. If you ever
inject a call from a debugger, you must respect it.

## 10. Unwinding: Following the `%rbp` Chain

Because each frame stores the previous `%rbp` at `[%rbp]` and the previous
`%rip` at `[%rbp+8]`, the frames form a linked list:

```
   frame 0 (innermost)          frame 1                     frame 2
   %rbp -> +--------------+
           | saved rbp  --|----> +--------------+
           | ret addr     |      | saved rbp  --|----> +--------------+
           | locals...    |      | ret addr     |      | saved rbp    |
           +--------------+      | locals...    |      | ret addr     |
                                 +--------------+      +--------------+

   pc[0] = %rip
   pc[1] = [%rbp + 8]                     rbp[1] = [%rbp]
   pc[2] = [rbp[1] + 8]                   rbp[2] = [rbp[1]]
   ...until rbp == 0
```

## 11. When `%rbp` Disappears: CFA and DWARF CFI

Under `-O1` and above (the flags this project builds with), GCC applies
`-fomit-frame-pointer`: there is no prologue, and `%rbp` becomes an ordinary
scratch register. The chain above is then **wrong**, not merely absent.

```
   WITH frame pointer                WITHOUT frame pointer  (-O1)

   push %rbp                         sub  $0x18,%rsp
   mov  %rsp,%rbp                    ...
   sub  $0x10,%rsp                   add  $0x18,%rsp
   ...                               ret
   leave
   ret                               %rbp holds... some variable
```

DWARF replaces it with the **CFA** (Canonical Frame Address): a per-PC rule for
computing a fixed anchor, canonically *the caller's `%rsp` just before the
`call`*.

```
      +----------------------+
      |  caller's frame      |
      +======================+ <-- CFA        anchor: constant for the frame
      |  return address      |     CFA - 8    "return address is at CFA-8"
      +----------------------+
      |  saved %rbp          |     CFA - 16   "rbp is saved at CFA-16"
      +----------------------+
      |  locals              |
      +----------------------+ <-- %rsp       moves; CFA does not
```

The CFI table gives one row per PC range, tracking how to recover the CFA as
the prologue executes:

```
   PC                        CFA rule            saved-register rules
   ---------------------------------------------------------------------
   foo+0   (entry)           CFA = %rsp + 8      ra   at CFA-8
   foo+1   (after push %rbp) CFA = %rsp + 16     ra   at CFA-8, rbp at CFA-16
   foo+4   (after mov)       CFA = %rbp + 16     ra   at CFA-8, rbp at CFA-16
   foo+8   (after sub)       CFA = %rbp + 16     (unchanged)
```

Unwinding one level = evaluate the rules at the current PC:

```
   CFA        = <evaluate rule using current %rsp / %rbp>
   caller %rip = [CFA - 8]         (the "return address column")
   caller %rsp = CFA               (by definition)
   caller %rbp = [CFA - 16]        (if a rule says it was saved)
```

Repeat with those values and you have the next frame — no frame pointer needed.

## 12. DWARF Register Numbers (x86-64)

The register numbering CFI rules refer to:

| DWARF # | register | note |
|---|---|---|
| 0–5 | rax, rdx, rcx, rbx, rsi, rdi | note the non-obvious order |
| 6 | **rbp** | |
| 7 | **rsp** | |
| 8–15 | r8–r15 | |
| 16 | **rip** | doubles as the *return address column* |

That last row is the link between this document's two halves: the return
address a debugger digs out of the stack *is* a saved `%rip`, so DWARF gives it
`%rip`'s register number.

## 13. `%rip`-Relative Addressing

x86-64 added an addressing mode that uses `%rip` as a base — this is how
position-independent code reaches globals without a relocation at runtime.

```
   0x1000:  48 8b 05 55 2e 00 00      mov  0x2e55(%rip),%rax
   0x1007:  <next instruction>
              ^
              |  the base is the address of the NEXT instruction
              |
   target = 0x1007 + 0x2e55 = 0x3e5c
```

The displacement is relative to the end of the current instruction, **not** its
start. This is the classic off-by-N when computing targets by hand — the
disassembler must know the instruction's length to resolve it.

## 14. `%rip` and Software Breakpoints

Setting a breakpoint means patching a one-byte `int3` (`0xcc`) over the first
byte of an instruction. The CPU advances `%rip` past the `int3` *before* the
trap is delivered, so the reported PC is one byte too high.

```
   original      0x4000:  55        push %rbp
   patched       0x4000:  cc        int3            saved_data_ = 0x55

   1. CPU fetches int3 at 0x4000
   2. %rip <- 0x4001                    (advanced past the 1-byte instruction)
   3. SIGTRAP raised, debugger wakes up

   4. debugger:  pc = get_pc() - 1      = 0x4000
                 set_pc(0x4000)         rewind so resume re-executes `push`
```

This rewind is *only* correct for **software** breakpoints. Hardware
breakpoints (DR0–DR3) do not modify code bytes and report the faulting address
directly, so no adjustment is applied.

## In This Codebase

| Concept | Location |
|---|---|
| `get_pc()` / `set_pc()` wrappers over `%rip` | `include/libgsdb/process.hpp:129,137` |
| Register table with DWARF numbers `rbp=6`, `rsp=7`, `rip=16` | `include/libgsdb/detail/registers.inc:34,35,44` |
| `%rbp`-chain unwind to find the return address | `src/target.cpp:220-226` (`target::step_out`) |
| `int3` PC rewind | `src/process.cpp:196-203` |
| Long-form explanation of the rewind | `src/breakpoint_site.cpp:45-58` |
| CIE / FDE / `.eh_frame_hdr` structures for CFI | `include/libgsdb/dwarf.hpp:25-80` |
| PC as a `file_addr` (bias-adjusted for DWARF lookup) | `src/target.cpp:67` |

Related notes: `x86-64-sub-registers.md`, `step_out-walkthrough.md`,
`register-xmacro-explained.md`.
