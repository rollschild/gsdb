# `target::step_out()` walkthrough

Location: `src/target.cpp:691-721`

`step_out()` implements the "finish this function, stop in the caller" command. It has
two paths: a virtual one for *inlined* functions (which have no real stack frame to
return from) and a physical one that uses the actual call stack.

```cpp
gsdb::stop_reason gsdb::target::step_out(std::optional<pid_t> otid) {
    auto tid = otid.value_or(process_->current_thread());
    auto& stack = get_stack(tid);
    auto inline_stack = stack.inline_stack_at_pc();
    auto has_inline_frames = inline_stack.size() > 1;
    auto at_inline_frame = stack.inline_height() < inline_stack.size() - 1;

    if (has_inline_frames and at_inline_frame) {
        auto current_frame =
            inline_stack[inline_stack.size() - stack.inline_height() - 1];
        auto return_address = current_frame.high_pc().to_virt_addr();
        return run_until_address(return_address, tid);
    }

    // grab the stack frame above this one, and retrieve the PC value for that
    // frame, which will be the return address for the current frame
    auto& regs = stack.frames()[stack.current_frame_index() + 1].regs;
    virt_addr return_address{
        regs.read_by_id_as<std::uint64_t>(register_id::rip)};
    stop_reason reason;
    // Keep running up to that address until the number of stack frames is less
    // than it was at the start of the stepping procedure
    for (auto frames = stack.frames().size();
         stack.frames().size() >= frames;) {
        reason = run_until_address(return_address, tid);
        if (!reason.is_breakpoint() or process_->get_pc() != return_address) {
            return reason;
        }
    }
    return reason;
}
```

## Setup: figuring out where we are (lines 692–696)

```cpp
auto tid = otid.value_or(process_->current_thread());
auto& stack = get_stack(tid);
auto inline_stack = stack.inline_stack_at_pc();
```

`tid` is the thread to step, the current thread unless the caller names one; each
thread has its own stack. `inline_stack_at_pc()` (`src/stack.cpp:13`) converts the current PC to a `file_addr` and
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

## Path 1: stepping out of an inlined function (lines 698–703)

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
return run_until_address(return_address, tid);
```

`run_until_address()` (`src/target.cpp:614`) plants a temporary *internal* breakpoint
at the address (unless one already exists there), resumes, waits for the stop, relabels
the trap as `single_step` if we stopped at the expected spot (so the CLI reports it
like a step rather than a breakpoint hit), and removes the temporary breakpoint.

**Subtlety**: this assumes the inlined function's code is a single contiguous range
ending at `high_pc`. If the compiler scattered it (`DW_AT_ranges`) or execution leaves
the range early, this heuristic can miss — a known limitation of the simple approach.

## Path 2: stepping out of a real function (lines 705–720)

```cpp
auto& regs = stack.frames()[stack.current_frame_index() + 1].regs;
virt_addr return_address{
    regs.read_by_id_as<std::uint64_t>(register_id::rip)};
```

The return address comes from the unwound call stack. On every stop,
`stack::unwind()` (`src/stack.cpp:58`) uses the DWARF CFI unwinder
(`call_frame_information::unwind()`) to rebuild each caller frame's registers. The frame
just above the current one is the caller, and its `rip` is where the current function
returns to. If there is no caller frame (for example in `main` when libc has no DWARF
info), `frames()[current_frame_index() + 1]` reads past the end of the frame list; the
bug audit tracks this as C4.

```cpp
for (auto frames = stack.frames().size();
     stack.frames().size() >= frames;) {
    reason = run_until_address(return_address, tid);
    if (!reason.is_breakpoint() or process_->get_pc() != return_address) {
        return reason;
    }
}
```

The loop is meant for recursion: if a deeper recursive call reaches the same return
address first, the frame count has not dropped yet, so it should run again. As written
it never repeats: `run_until_address()` relabels a stop at the target address as
`single_step`, so `is_breakpoint()` is false and the loop returns after the first stop.

### Earlier approach: the saved return address at `rbp + 8`

Until `ef32937` ("expose stack unwinding to the user", 2026-08-19), `step_out()` read the
return address from the frame-pointer chain instead:

```cpp
auto frame_pointer = process_->get_registers().read_by_id_as<std::uint64_t>(
    register_id::rbp);
```

This read `rbp` from the cached register state (populated by `PTRACE_GETREGS` on the
last stop).

```cpp
auto return_address =
    process_->read_memory_as<std::uint64_t>(virt_addr{frame_pointer + 8});
```

This relies on the standard x86-64 frame-pointer prologue. When a function is called
and runs `pushq %rbp; movq %rsp, %rbp`, the stack looks like:

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
│   return address            │  ← rbp + 8   ← what the old step_out() read
│                             │    (pushed by the caller's `call` instruction)
├─────────────────────────────┤
│   caller's saved rbp        │  ← rbp + 0   ← rbp points HERE
│                             │    (pushed by the callee's `pushq %rbp`;
│                             │     `movq %rsp, %rbp` then anchors rbp here)
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
`pushq %rbp` saves the caller's frame pointer one slot below it; `movq %rsp, %rbp`
anchors `rbp` at that slot. From that fixed anchor, `(%rbp)` is the caller's saved
`rbp` (the link for walking the frame chain) and `8(%rbp)` is the return
address — `+8` because it's one 8-byte slot toward higher addresses, i.e. one
slot earlier in push order.

**Caveat**: this only works for code compiled with frame pointers — with
`-fomit-frame-pointer`, `rbp` is a general-purpose register and this would read
garbage. That is why `step_out()` now takes the return address from the CFI unwinder
(`call_frame_information::unwind()`, driven by `stack::unwind()` in
`src/stack.cpp:58`), as described above.

```cpp
return run_until_address(virt_addr{return_address});
```

The old version then used the same temporary-breakpoint mechanism: run until we land on
the return address in the caller, and report the stop.

## Why not just breakpoint the return address in both cases?

Because for an inlined "call" there *is* no return address on the stack — the whole
point of the branch at line 698 is that inline frames are a fiction reconstructed from
DWARF, so stepping out of one is simulated by running to the end of its code range,
while stepping out of a real frame uses the genuine saved return address.
