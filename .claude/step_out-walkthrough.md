# `target::step_out()` walkthrough

Location: `src/target.cpp:204-224`

`step_out()` implements the "finish this function, stop in the caller" command. It has
two paths: a virtual one for *inlined* functions (which have no real stack frame to
return from) and a physical one that uses the actual call stack.

```cpp
gsdb::stop_reason gsdb::target::step_out() {
    auto& stack = get_stack();
    auto inline_stack = stack.inline_stack_at_pc();
    auto has_inline_frames = inline_stack.size() > 1;
    auto at_inline_frame = stack.inline_height() < inline_stack.size() - 1;

    if (has_inline_frames and at_inline_frame) {
        auto current_frame =
            inline_stack[inline_stack.size() - stack.inline_height() - 1];
        auto return_address = current_frame.high_pc().to_virt_addr();
        return run_until_address(return_address);
    }

    auto frame_pointer = process_->get_registers().read_by_id_as<std::uint64_t>(
        register_id::rbp);

    auto return_address =
        process_->read_memory_as<std::uint64_t>(virt_addr{frame_pointer + 8});

    return run_until_address(virt_addr{return_address});
}
```

## Setup: figuring out where we are (lines 205–208)

```cpp
auto& stack = get_stack();
auto inline_stack = stack.inline_stack_at_pc();
```

`inline_stack_at_pc()` (`src/stack.cpp:7`) converts the current PC to a `file_addr` and
asks the DWARF parser for the chain of functions at that address. The result is a
vector of `die`s ordered **outermost-first**: index 0 is the real, physical function
(`DW_TAG_subprogram`), and each following element is a nested
`DW_TAG_inlined_subroutine`. The last element is the deepest inlined function
containing the PC. If nothing was inlined here, the vector has exactly one element.

```cpp
auto has_inline_frames = inline_stack.size() > 1;
```

True when the PC sits inside at least one inlined function.

```cpp
auto at_inline_frame = stack.inline_height() < inline_stack.size() - 1;
```

`inline_height()` is the debugger's cursor into that inline chain: 0 means the user is
"in" the deepest inline frame, and larger values move up toward the physical function.
(It gets set by `reset_inline_height()` when the PC lands exactly on an inlined
function's first instruction, letting `step` pretend it stopped *before* entering the
inlined code.) The outermost frame is at height `size - 1`, so `height < size - 1`
means the user's current conceptual frame is one of the inlined ones — not the physical
function.

## Path 1: stepping out of an inlined function (lines 210–215)

An inlined function has no `call`/`ret` and no frame of its own — its instructions are
spliced into the caller's body. So "step out" can't pop a stack frame; instead:

```cpp
auto current_frame =
    inline_stack[inline_stack.size() - stack.inline_height() - 1];
```

This indexes the vector to get the DIE of the frame the user is currently in. Height 0
→ last element (deepest); height `size - 2` → index 1; etc. It's the mirror-image
arithmetic needed because the vector is outermost-first but height counts from the
deepest end.

```cpp
auto return_address = current_frame.high_pc().to_virt_addr();
```

`high_pc()` is the DWARF attribute giving one-past-the-end of the inlined function's
code range. The first instruction *after* the inlined body is exactly where execution
"returns" to the inlining caller, so that's the target. `to_virt_addr()` adds the load
bias to turn the file-relative address into a runtime address (PIE binaries load at a
random base).

```cpp
return run_until_address(return_address);
```

`run_until_address()` (`src/target.cpp:137`) plants a temporary *internal* breakpoint
at the address (unless one already exists there), resumes, waits for the stop, relabels
the trap as `single_step` if we stopped at the expected spot (so the CLI reports it
like a step rather than a breakpoint hit), and removes the temporary breakpoint.

**Subtlety**: this assumes the inlined function's code is a single contiguous range
ending at `high_pc`. If the compiler scattered it (`DW_AT_ranges`) or execution leaves
the range early, this heuristic can miss — a known limitation of the simple approach.

## Path 2: stepping out of a real function (lines 217–223)

```cpp
auto frame_pointer = process_->get_registers().read_by_id_as<std::uint64_t>(
    register_id::rbp);
```

Reads `rbp` from the cached register state (populated by `PTRACE_GETREGS` on the last
stop).

```cpp
auto return_address =
    process_->read_memory_as<std::uint64_t>(virt_addr{frame_pointer + 8});
```

This relies on the standard x86-64 frame-pointer prologue. When a function is called
and runs `push rbp; mov rbp, rsp`, the stack looks like:

```
rbp + 8  → return address   (pushed by the caller's `call`)
rbp + 0  → caller's saved rbp
```

So the caller's return address lives 8 bytes above the frame pointer, and reading
8 bytes of inferior memory at `rbp + 8` fetches it.

Full picture (the x86-64 stack grows downward, toward lower addresses, so the
caller's data sits at *higher* addresses than the callee's):

```
 High addresses
┌─────────────────────────────┐
│   caller's stack frame      │
│   (locals, spills, …)       │
├─────────────────────────────┤
│   arg 7, arg 8, …           │  ← only if the call has more than 6
│   (stack-passed arguments)  │    integer args (first 6 go in registers)
├─────────────────────────────┤
│   return address            │  ← rbp + 8   ← what step_out() reads
│                             │    (pushed by the caller's `call` instruction)
├─────────────────────────────┤
│   caller's saved rbp        │  ← rbp + 0   ← rbp points HERE
│                             │    (pushed by the callee's `push rbp`;
│                             │     `mov rbp, rsp` then anchors rbp here)
├─────────────────────────────┤
│   callee's local variables  │  ← rbp - 8, rbp - 16, …
│   saved callee-saved regs   │
├─────────────────────────────┤
│   (red zone / scratch)      │  ← rsp points somewhere at/below here
└─────────────────────────────┘
 Low addresses
        │
        ▼  stack grows this way (push = rsp -= 8)
```

In execution order: the caller's `call` pushes the return address; the callee's
`push rbp` saves the caller's frame pointer one slot below it; `mov rbp, rsp`
anchors `rbp` at that slot. From that fixed anchor, `[rbp]` is the caller's saved
`rbp` (the link for walking the frame chain) and `[rbp + 8]` is the return
address — `+8` because it's one 8-byte slot toward higher addresses, i.e. one
slot earlier in push order.

**Caveat**: this only works for code compiled with frame pointers — with
`-fomit-frame-pointer`, `rbp` is a general-purpose register and this would read
garbage; a full debugger would use DWARF CFI unwind info instead.

```cpp
return run_until_address(virt_addr{return_address});
```

Same temporary-breakpoint mechanism: run until we land on the return address in the
caller, and report the stop.

## Why not just breakpoint the return address in both cases?

Because for an inlined "call" there *is* no return address on the stack — the whole
point of the branch at line 210 is that inline frames are a fiction reconstructed from
DWARF, so stepping out of one is simulated by running to the end of its code range,
while stepping out of a real frame uses the genuine saved return address.
