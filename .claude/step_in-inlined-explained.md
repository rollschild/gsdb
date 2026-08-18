# `target::step_in()` — the inlined-frame fast path explained

Location: `src/target.cpp:82` (the early `if` block at lines 84–88)

```cpp
gsdb::stop_reason gsdb::target::step_in() {
    auto& stack = get_stack();
    if (stack.inline_height() > 0) {
        stack.simulate_inlined_step_in();   // --inline_height_, then resync current_frame_
        return stop_reason(process_state::stopped, SIGTRAP,
                           trap_type::single_step);
    }
    // ... real instruction-stepping logic below ...
}
```

And `simulate_inlined_step_in()` (`include/libgsdb/stack.hpp:39`) is just the
decrement plus a resync of the frame cursor:

```cpp
void simulate_inlined_step_in() {
    --inline_height_;
    current_frame_ = inline_height_;
}
```

## The setup

This is the first thing `target::step_in()` (the "step into" command) checks:
is the current PC parked at the entry of one or more inlined functions?

Recall what `inline_height_` means (computed by `reset_inline_height()` every
time the process halts — see `reset_inline_height-explained.md`):

> The PC sits on a single physical address that is the **entry point of several
> nested inlined functions**. `inline_height_` is how many of those deepest
> inlined frames you're conceptually "at the door of" but haven't entered yet.

```
  pc = 0x1140  (entry of both bar() and baz())     inline_height_ = 2

  main()
   └─ foo()        ← currently presented frame
       └─ bar()    ← at entry, not yet entered   ┐ height
           └─ baz()    ← at entry, not yet entered ┘  = 2
```

## Why we "stop" at the inlined function here

The crucial point: **all of these inlined frames share the same machine
instruction.** There is no separate `call` instruction to execute to "enter"
`bar()` — its code is already inlined right there. So if you issued a real
`step_instruction()` (PTRACE single-step, the path lower down in the function),
the PC would move *past* the entry and skip the user's chance to see themselves
"inside" `bar()`.

So step-into has to be **simulated** at these points:

- A normal step into a real function = execute a `call`, land on the callee's
  first line.
- A step into an *inlined* function = **don't move the PC at all**; just move the
  debugger's logical cursor one frame deeper by decrementing `inline_height_`.

That's why it returns immediately with a synthetic `single_step` stop reason and
**never touches the process**. From the user's perspective they "stepped into"
`bar()`, but physically nothing executed — only the displayed frame changed.

```
  step_in()  with inline_height_ = 2
     │
     ├─ inline_height_ > 0 ?  yes
     │     --inline_height_   (now 1)         PC unchanged
     │     return single_step  ───────────►   user now "inside" bar()
     │
  step_in()  again, inline_height_ = 1
     │     --inline_height_   (now 0)         PC unchanged
     │     return single_step  ───────────►   user now "inside" baz() (deepest)
     │
  step_in()  again, inline_height_ = 0
     │     falls through to the REAL stepping logic below (line 90+)
     │     actually single-steps machine instructions
```

## TL;DR

- `inline_height_ > 0` means the PC is sitting at the shared entry instruction of
  one or more inlined calls you haven't logically entered yet.
- You can't single-step *into* inlined code — there's no `call` to execute; the
  code is already here.
- So `step_in()` fakes it: decrement the inline cursor, report a single-step
  stop, and leave the process untouched.
- Once `inline_height_` reaches 0 (you've "descended" into the deepest inline
  frame), the next `step_in()` falls through to the real instruction-stepping
  loop.
