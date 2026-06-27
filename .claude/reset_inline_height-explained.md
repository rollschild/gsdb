# `stack::reset_inline_height()` explained

Location: `src/stack.cpp:21`

```cpp
void gsdb::stack::reset_inline_height() {
    auto stack = inline_stack_at_pc();

    inline_height_ = 0;  // pointing to the deepest inline function
    auto pc = target_->get_pc_file_address();

    // reverse iterators - iterate backward
    // starting at the deepest one, until we hit either the beginning or a frame
    // of which execution isn't at the start
    for (auto it = stack.rbegin(); it != stack.rend() and it->low_pc() == pc;
         ++it) {
        ++inline_height_;
    }
}
```

This function sets a cursor (`inline_height_`) that tracks **which inlined frame
the debugger should currently present** when several inlined functions overlap
the same physical PC.

## Background: how inlining looks in DWARF

When the compiler inlines functions, several source-level "frames" collapse onto
the **same machine address**. DWARF records this with nested
`DW_TAG_inlined_subroutine` DIEs. `inline_stack_at_pc()` (which delegates to
`dwarf::inline_stack_at_address()`, `src/dwarf.cpp:1088`) flattens that nesting
into a vector:

```
inline_stack_at_pc()  returns:

  index 0   ┌────────────────────────────┐   real (non-inlined) function
            │  main()                    │   stack[0]   ← outermost
            ├────────────────────────────┤
  index 1   │  inlined foo()             │   stack[1]
            ├────────────────────────────┤
  index 2   │  inlined bar()             │   stack[2]
            ├────────────────────────────┤
  index 3   │  inlined baz()             │   stack[3]   ← deepest / innermost
            └────────────────────────────┘

  vector order:  outermost ──────────────► deepest
```

All of these DIEs "contain" the current PC. The deepest one is the most
specific.

How it is built (`dwarf::inline_stack_at_address`):

1. Find the real `DW_TAG_subprogram` containing the address; push it as `stack[0]`.
2. Repeatedly scan the current frame's children for a
   `DW_TAG_inlined_subroutine` whose range contains the address; push it and
   descend. Stop when no such child exists.

## What `reset_inline_height()` computes

It walks that vector **in reverse** (deepest → outermost) and counts how many
frames, *consecutively starting from the deepest*, have `low_pc() == pc` — i.e.
the PC sits **exactly on that inlined function's first instruction**.

```
                          low_pc()      pc == low_pc() ?
  stack.rbegin() ─► baz()  0x1140         0x1140  ✓   ++inline_height_ (=1)
                    bar()  0x1140         0x1140  ✓   ++inline_height_ (=2)
                    foo()  0x1130         0x1140  ✗   STOP
                    main() 0x1100          ...        (never reached)
  stack.rend()
```

The loop's guard is `it != stack.rend() and it->low_pc() == pc`. The moment a
frame's start address differs from the PC (execution is *inside* it, not *at its
entry*), the loop stops.

## Why "execution is at the beginning" matters

If `pc == low_pc` of an inlined routine, then at the **source level you haven't
really entered that inline call yet** — you're parked on its first instruction.
Conceptually the user should be looking at the *caller's* frame and be able to
"step into" the inline. So those entry-point frames get counted as a height
offset.

```
  pc = 0x1140  (entry of both bar and baz)

  Physical view (one PC)         Source/logical view the debugger presents
  ──────────────────────         ─────────────────────────────────────────
                                  main()
                                   └─ foo()        ← we're conceptually HERE
                                       └─ bar()    ← at entry, not yet "in"   ┐ height
                                           └─ baz() ← at entry, not yet "in"   ┘  = 2
```

So `inline_height_ = 2` means: "two of the deepest inlined frames are sitting at
their entry point; present the frame two levels up from the deepest."

## Lifecycle

Per the header (`include/libgsdb/stack.hpp:14`), this is **called every time the
process halts**. Each stop recomputes the cursor from scratch:

```
process stops ─► wait_on_signal ─► reset_inline_height()
                                       │
                                       ├─ inline_height_ = 0   (point at deepest)
                                       └─ count entry-point frames upward
```

`inline_height()` then exposes the value, and other code (e.g. an `up`/`down`
stepping command or the stop-message formatter) uses it to decide which inlined
function name/source line to show and how a subsequent step should behave.

## TL;DR

| Element | Meaning |
|---|---|
| `inline_stack_at_pc()` | outermost → deepest list of DIEs covering the PC |
| `inline_height_ = 0` | cursor points at the **deepest** inlined frame |
| the reverse loop | counts deepest-first frames whose **start address == PC** |
| result | how many inlined frames are "at their entry, not yet entered" — the offset used to pick which logical frame to display |

It's the bookkeeping that lets the debugger show sensible inline-aware stack
frames instead of dumping you onto a single raw machine address shared by several
inlined calls.
