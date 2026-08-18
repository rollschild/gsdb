# `target::step_over()` — Explained

Source: `src/target.cpp:166-208`

## What it is supposed to do

"Step over" advances the inferior by **one source line** of the *current*
function, and treats any function calls (both real `call` instructions and
**inlined** function bodies) as a single atomic step — they run to completion
without the debugger descending into them.

The loop keeps stepping until the program counter lands on a **new, real line
table entry** (one that is not an `end_sequence` sentinel).

---

## The three step strategies

On every iteration, exactly one of three things happens depending on where the
PC currently sits:

```
                 ┌──────────────────────────────┐
                 │  Where is the PC right now?  │
                 └──────────────────────────────┘
                              │
        ┌─────────────────────┼─────────────────────────┐
        ▼                     ▼                          ▼
 (A) at the start of    (B) on a real `call`     (C) anything else
     an INLINED frame       instruction              (ordinary insn)
        │                     │                          │
        ▼                     ▼                          ▼
 run_until_address(      run_until_address(        process_->
   frame.high_pc )         next_insn_addr )          step_instruction()
        │                     │                          │
   skip the whole        let the callee run         single-step one
   inlined body          at full speed, stop        CPU instruction
   in one shot           at the return addr
```

### (A) Inlined frame — `target.cpp:174-185`

```cpp
auto inline_stack = stack.inline_stack_at_pc();
auto at_start_of_inline_frame = stack.inline_height() > 0;
if (at_start_of_inline_frame) {
    auto frame_to_skip =
        inline_stack[inline_stack.size() - stack.inline_height()];
    auto return_address = frame_to_skip.high_pc().to_virt_addr();
    reason = run_until_address(return_address);
    ...
}
```

Inlined functions have no `call` instruction to detect — the compiler spliced
the callee's body directly into the caller's machine code. DWARF records the
splice as `DW_TAG_inlined_subroutine` DIEs, which `inline_stack_at_pc()`
reconstructs as a vector of frames.

`inline_height_` is the cursor into that virtual stack (managed by
`stack::reset_inline_height()`, which counts how many inlined frames begin
*exactly* at the current PC). When `inline_height() > 0`, the PC is sitting at
the entrance of an inlined body, so we pick that frame and jump straight to its
`high_pc` (one-past-the-end of the inlined region) — skipping the entire
inlined body in a single `run_until_address`.

```
   inline_stack (outermost ──► innermost):
   ┌──────────┬──────────┬──────────┐
   │ frame[0] │ frame[1] │ frame[2] │
   └──────────┴──────────┴──────────┘
                          ▲
        index = size - inline_height
        (the frame we are currently entering, to be skipped)

   skip target = frame_to_skip.high_pc()  ──►  one past the inlined body
```

### (B) Real `call` instruction — `target.cpp:186-193`

```cpp
} else if (auto instructions = disas.disassemble(2, process_->get_pc());
           /* instructions[0].text.rfind("call") == 0*/ instructions[0]
               .text.starts_with("call")) {
    reason = run_until_address(instructions[1].address);
    ...
}
```

Disassemble **2** instructions starting at the PC:
- `instructions[0]` — the current instruction. `text.starts_with("call")` is the
  test (the older `rfind("call") == 0` idiom — "find the substring at offset 0" —
  is still there, commented out).
- `instructions[1]` — the very next instruction, i.e. the **return address**.

If the current instruction is a `call`, we don't single-step into the callee.
Instead `run_until_address(instructions[1].address)` plants a temporary
breakpoint at the return address, resumes at full speed, and stops once the
callee returns — stepping *over* it.

```
   memory:   ... | call foo | mov ... | ...
                  ^           ^
                  PC          instructions[1].address  (return address)
                  │           │
                  └─ run_until_address(─┘  ← run callee, stop here
```

### (C) Ordinary instruction — `target.cpp:194-199`

```cpp
} else {
    reason = process_->step_instruction();
    if (!reason.is_step()) {
        return reason;
    }
}
```

Not a call, not an inline entry → just single-step one machine instruction.

---

## `run_until_address()` — the "step over" primitive (`target.cpp:143-164`)

Both branch (A) and branch (B) lean on this helper:

```
run_until_address(addr):
   1. if no breakpoint already at addr:
        create an INTERNAL (hidden) software breakpoint there, enable it
   2. process_->resume()                 ← run at full speed
   3. reason = process_->wait_on_signal()
   4. if we stopped because of THAT breakpoint at addr:
        rewrite the stop reason as `single_step`   ← make it look like a step
   5. remove the temporary breakpoint (if we created it)
   6. return reason
```

The `internal=true` flag (3rd arg to `create_breakpoint_site`) keeps this
breakpoint out of the user's `breakpoint list`. Re-labelling the stop reason as
`single_step` (step 4) is what lets the caller's `reason.is_step()` checks pass
— from the outside, stepping over a call looks identical to a single step.

> ⚠️ Note: the breakpoint-at-return-address technique does **not** guard against
> recursion (a frame on the stack twice). A production debugger would also check
> the stack pointer at the moment the breakpoint fires. gsdb currently does not.

---

## The early-exit checks

After branch (A) or (B), the code guards:

```cpp
if (!reason.is_step() or process_->get_pc() != return_address) {
    return reason;
}
```

This means: *"if running to the return address didn't end in a clean step (e.g.
we hit a user breakpoint inside the callee, or the process exited), or we didn't
actually land where we aimed, bail out now and report that."* Step-over must
yield control to the user if something more interesting happened mid-call.

---

## The loop condition — `target.cpp:200-205`

```cpp
} while ((line_entry_at_pc() == orig_line or
          line_entry_at_pc()->end_sequence) and
         line_entry_at_pc() != line_table::iterator{});
```

Keep looping while **all** of:

| Condition | Meaning |
|---|---|
| `line_entry_at_pc() == orig_line` | still on the same source line we started on |
| `... or ->end_sequence` | …or parked on an `end_sequence` marker (a gap between functions / end of a line-table range — not a real line to stop on) |
| `... != line_table::iterator{}` | and we *have* a valid line entry at all (a default iterator means "no line info" — e.g. PC is in a shared library with no DWARF; stop looping in that case) |

Stop the loop (i.e. step-over is done) the moment the PC reaches a **different,
real, non-end-sequence line entry**.

```
   start ──► [ orig_line ] ──► [ orig_line ] ──► [ end_sequence ] ──► [ NEW line ]
             keep going        keep going        keep going          STOP, return
```

`line_entry_at_pc()` (`target.cpp:131-141`) maps the current PC (as a
**file address**, bias-adjusted) to a line-table row inside the correct compile
unit. It returns a default/empty iterator when there's no line info — which is
the third condition's escape hatch.

---

## End-to-end flow

```
 step_over()
    │
    │  orig_line = line_entry_at_pc()        ← remember where we started
    │
    ▼
 ┌──── do ───────────────────────────────────────────────┐
 │  inline frame at PC?  ──yes──► run_until inline high_pc │
 │        │ no                                             │
 │  current insn is `call`? ──yes──► run_until return addr │
 │        │ no                                             │
 │  single-step one instruction                           │
 │        │                                               │
 │  (any branch) did we stop for a non-step reason or      │
 │   miss our target?  ──yes──► return reason  (bail)     │
 └──── while: still on orig_line / end_sequence,           │
              and line info is valid ─────────────────────┘
    │
    ▼
 return reason     ← landed on a new source line in the same frame
```

---

## TL;DR

`step_over()` repeatedly advances the inferior — **skipping over inlined frames
and real `call`s** by running to their return point with a hidden internal
breakpoint, and single-stepping everything else — until the program counter
reaches a **new, real source line** (not the original line, not an
`end_sequence` sentinel). It bails early and hands control back if anything more
important (a user breakpoint, an exit) happens along the way.
