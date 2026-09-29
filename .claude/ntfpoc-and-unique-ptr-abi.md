# NTFPOC — "Non-Trivial For the Purposes Of Calls"

> Explains the Itanium C++ ABI rule quoted in `README.md` (Expression Evaluation →
> Calling Conventions → Classifying Parameters): what it means, why it exists,
> what it means for gsdb's expression evaluator, and why passing
> `std::unique_ptr` by value is a well-known performance gotcha.

---

## The rule

> **Non-trivial for the purposes of calls (NTFPOC)**: As defined in the Itanium
> ABI, a type is NTFPOC if it has a non-trivial copy constructor, move
> constructor, or destructor, or if all of its copy and move constructors have
> been deleted.

This is the C++ ABI's override that decides whether a class object may be passed
like a plain C struct (bits copied into registers or onto the stack) or must be
passed **by address**. The SysV parameter classes (INTEGER, SSE, …, MEMORY) only
apply to types that are *not* NTFPOC. A C++ compiler checks NTFPOC first; if the
answer is yes, it skips SysV classification for that value entirely.

---

## What changes

**Parameter:** the caller makes the copy in its own memory and passes a *pointer*
to it in the next integer register. The object is replaced in the argument list
by a pointer, which is class INTEGER.

```cpp
struct plain { long a, b; };            // trivial
struct owned { long a, b; ~owned(); };  // NTFPOC (user-provided dtor)

long f(plain p);
long g(owned o);
```

```asm
# f(p): two INTEGER eightbytes, passed in registers
movq  -16(%rbp), %rdi
movq  -8(%rbp), %rsi
call  f

# g(o): the caller's temporary copy sits at -32(%rbp); only its address is passed
leaq  -32(%rbp), %rdi
call  g
leaq  -32(%rbp), %rdi
call  _ZN5ownedD1Ev     # the caller destroys the temporary after the call
```

**Return value:** the caller reserves space and passes its address as a hidden
first argument in `%rdi`, so the real arguments move up one register. The callee
builds the result there and returns the same address in `%rax`. A trivial
`plain` would instead come back in `%rax:%rdx`.

**NTFPOC is not the MEMORY class.** A MEMORY-class argument (e.g. a trivial
24-byte struct) is copied *by value* into the stack argument area. An NTFPOC
argument takes up only one pointer-sized register slot. Only for return values
do the two look the same, since both use the hidden pointer.

---

## Why the rule exists

Putting an object in registers is a bitwise copy, and the copy then ends up at a
different address. That's only safe when copying the bits is all a copy means.

- **Non-trivial copy/move constructor:** the language says a copy must run that
  constructor, and the object may depend on where it lives. For example,
  libstdc++'s `std::string` stores a pointer to its own inline buffer when the
  string is short (small-string optimization). If you copy its bits somewhere
  else, that pointer still points at the old object.
- **Non-trivial destructor:** something has to run the destructor on the
  parameter at a known address. Under Itanium the *caller* does this, so the
  object has to live in the caller's memory.
- **All copy/move constructors deleted:** the type can't be copied at all, so
  there's no legal way to "copy it into registers." It has to stay where it was
  built (C++17 guaranteed elision constructs it in place).

---

## What "trivial" means here

Only three special members count: the copy constructor, the move constructor,
and the destructor. One of them is trivial if:

1. it is not user-provided (it is implicit, or `= default` **inside the class
   body**), **and**
2. the class has no virtual functions or virtual bases (a copy would have to set
   up the vptr), **and**
3. the same member is trivial in every base class and every non-static data
   member, so the property carries over to containing types.

| Type | NTFPOC? | Why |
|---|---|---|
| `struct { int x, y; }` | no | everything implicit |
| `struct T { int x; ~T() {} };` | **yes** | user-provided dtor, even an empty one |
| `struct T { int x; T(const T&) = default; };` | no | defaulted in-class |
| `T::T(const T&) = default;` (out of class) | **yes** | defaulting outside the class counts as user-provided |
| `struct { virtual void f(); int x; }` | **yes** | vptr |
| `struct { std::string s; }` | **yes** | the member's copy ctor is non-trivial |
| `std::unique_ptr<int>` by value | **yes** | non-trivial move ctor and dtor (see below) |
| `T(const T&) = delete; T(T&&) = delete;` | **yes** | all copy/move ctors deleted |
| `T(const T&) = delete; T(T&&) = default;` | no | a deleted function isn't user-provided, so both ctors are trivial, and not *all* are deleted |
| `struct T { T() {} int x; };` | no | the default ctor and assignment operators don't matter |

---

## What it means for gsdb's evaluator

The README lists the evaluator steps; in step 3 (set up stack and registers) and
step 8 (read the result), test each class-type argument and the return type for
NTFPOC **before** SysV classification:

- **Argument:** write the object into inferior memory and pass its address as an
  INTEGER argument. Strictly speaking the copy should go through the copy
  constructor, not a raw byte copy.
- **Return:** reserve space, put its address in `%rdi`, shift the other
  arguments along one register, then read the result from the address in `%rax`.

### Detecting NTFPOC from DWARF 4

Look at the `DW_TAG_subprogram` children of the class DIE (constructors and
destructor). A special member counts as trivial if it is:

- **missing** (never declared, so the compiler would declare it implicitly), or
- marked `DW_AT_artificial` (implicitly declared), or
- marked `DW_AT_defaulted == DW_DEFAULTED_in_class`.

`DW_DEFAULTED_out_of_class` means user-provided, so it's non-trivial. That
distinction is probably why `DW_AT_defaulted` is already in
`include/libgsdb/detail/dwarf.h` (the "From DWARF5, but GCC still outputs in
DWARF4 mode" block).

You also need to:

- follow `DW_TAG_inheritance` and `DW_TAG_member` types for the carry-over rule
  (rule 3 above), and
- treat any virtual member function or virtual base as non-trivial.

**Gap:** `dwarf.h` doesn't define `DW_AT_deleted` (0x8a), which I believe GCC
also emits in non-strict DWARF 4 mode. Without it, a deleted constructor looks
user-provided. That gives the right answer for the all-deleted case by accident,
but the wrong answer for `T(const T&) = delete; T(T&&) = default;`, which is
**not** NTFPOC and goes in registers.

---

## Follow-up: why is `std::unique_ptr` by value a performance gotcha?

It looks free and it isn't. `std::unique_ptr<int>` with the default deleter is
just an `int*` wrapped in a class: 8 bytes, and the empty deleter takes no space.
So people assume `f(std::unique_ptr<int>)` costs the same as `f(int*)`, with the
pointer passed in `%rdi`.

The ABI works differently. Moving a `unique_ptr` has to null out the source, and
destroying one has to call `delete`. That gives it a non-trivial move
constructor and destructor, which makes it **NTFPOC**. So it's passed by
invisible reference, as described above. Its size doesn't matter.

### What that costs

GCC 15.2, `-O2`, with exceptions and the Nix hardening flags turned off so the
output is readable:

```cpp
#include <memory>

void take_raw(int* p);
void take_up(std::unique_ptr<int> p);

void pass_raw(int* p)                 { take_raw(p); }
void pass_up(std::unique_ptr<int>& p) { take_up(std::move(p)); }

int read_raw(int* p)                { return *p; }
int read_up(std::unique_ptr<int> p) { return *p; }
```

```asm
pass_raw(int*):
    jmp   take_raw(int*)            # tail call, nothing else

pass_up(std::unique_ptr<int>&):
    subq  $24, %rsp                 # stack slot for the temporary
    movq  (%rdi), %rax              # move-construct the temporary:
    movq  $0, (%rdi)                #   null out the source,
    leaq  8(%rsp), %rdi             #   pass the temporary's *address*
    movq  %rax, 8(%rsp)             #   (its value is the old pointer)
    call  take_up                   # can't tail-call: a destructor is still owed
    movq  8(%rsp), %rdi             # ~unique_ptr() on the temporary:
    testq %rdi, %rdi                #   did the callee move from it?
    je    .L3
    movl  $4, %esi
    call  operator delete(void*, unsigned long)
.L3:
    addq  $24, %rsp
    ret

read_raw(int*):
    movl  (%rdi), %eax
    ret

read_up(std::unique_ptr<int>):
    movq  (%rdi), %rax              # extra load: %rdi points to the unique_ptr...
    movl  (%rax), %eax              # ...which points to the int
    ret
```

To reproduce (outside the source tree):

```bash
g++ -std=c++23 -O2 -S -o - up.cpp \
    -fno-asynchronous-unwind-tables -fcf-protection=none \
    -fno-stack-protector -fzero-call-used-regs=skip \
    -fomit-frame-pointer -fno-exceptions | c++filt
```

The extra flags only strip noise. The Nix GCC wrapper turns on stack protector,
frame pointers, and `-fzero-call-used-regs` (the trailing `xorl %edi, %edi`
lines) by default.

### Walkthrough: `pass_up` one instruction at a time

`pass_up` calls `take_up(std::move(p))`. Because `unique_ptr` is passed by
invisible reference, `pass_up` has to build a real `unique_ptr` object in its
own stack frame, pass that object's address, and destroy it after the call.

The diagrams use these example values. The addresses are made up, but the
alignment is realistic.

| Thing | Example value |
|---|---|
| Caller's `p` (the `unique_ptr&` argument) lives at | `0x7ffe0a0` |
| `p` holds (the heap `int*`) | `0x4052a0` |
| `%rsp` on entry to `pass_up` | `0x7ffe058` (ends in `8`, because `call` just pushed an 8-byte return address) |

Stack diagrams have high addresses at the top, and the stack grows downward.

#### Entry (before `subq`)

```
                 ┌────────────────────────────┐
     0x7ffe0a0   │ p = 0x4052a0               │  caller's unique_ptr ──────┐
                 │   ... caller's frame ...   │                            │
                 ├────────────────────────────┤                            ▼
     0x7ffe058   │ return addr → caller       │ ◄── %rsp          ┌─────────────┐
                 └────────────────────────────┘                   │ heap: int   │
                                                                  │ @ 0x4052a0  │
   %rdi = 0x7ffe0a0  (&p, since p was passed by reference)        └─────────────┘
   %rax = ?
```

#### `subq $24, %rsp`: make room for the temporary

```
     0x7ffe058   │ return addr → caller       │
                 ├────────────────────────────┤ ─┐
     0x7ffe050   │ ?? (padding)               │  │ 16(%rsp)
     0x7ffe048   │ ?? ← temp unique_ptr slot  │  │  8(%rsp)     24 bytes, pass_up's frame
     0x7ffe040   │ ?? (padding)               │  │  0(%rsp) ◄── %rsp
                 └────────────────────────────┘ ─┘
```

This does two things:

- **Reserves 8 bytes for the temporary** at `8(%rsp)`. The temporary is the
  by-value parameter object that `take_up` will receive.
- **Realigns the stack.** `0x…058 − 24 = 0x…040`, which is 16-byte aligned. The
  SysV ABI requires 16-byte alignment at every `call`, and there are two `call`s
  ahead. Subtracting 8 would also have aligned it, so the extra 16 bytes of
  padding are just a GCC frame-layout choice. Only the middle slot is used.

#### `movq (%rdi), %rax`: read the pointer out of `p`

```
     0x7ffe0a0   │ p = 0x4052a0               │
                 ⋮
     0x7ffe048   │ ?? temp                    │
     0x7ffe040   │ ??                         │ ◄── %rsp

   %rdi = 0x7ffe0a0   %rax = 0x4052a0   ← pointer copied into a register
```

This is the first half of the move constructor, `temp.ptr = src.ptr`.

#### `movq $0, (%rdi)`: null out the source

```
     0x7ffe0a0   │ p = 0  (nullptr)           │  ← caller's p is now empty
                 ⋮
     0x7ffe048   │ ?? temp                    │
     0x7ffe040   │ ??                         │ ◄── %rsp

   %rax = 0x4052a0   ← for a moment, the ONLY copy of the owning pointer is in a register
```

This is the second half of the move constructor, `src.ptr = nullptr`. It's the
reason `unique_ptr` has a non-trivial move constructor: a raw `int*` copy would
skip this store.

#### `leaq 8(%rsp), %rdi`: compute the temporary's address as argument 1

```
     0x7ffe048   │ ?? temp                    │ ◄──┐
     0x7ffe040   │ ??                         │    │ ◄── %rsp
                                                   │
   %rdi = 0x7ffe048 ───────────────────────────────┘   (just an address, no memory access)
   %rax = 0x4052a0
```

`leaq` only does arithmetic (`%rsp + 8`) and never touches memory. This is the
invisible reference: the argument register holds the address of a `unique_ptr`,
not the `int*` inside it.

#### `movq %rax, 8(%rsp)`: finish constructing the temporary

```
     0x7ffe0a0   │ p = 0                      │
                 ⋮
     0x7ffe058   │ return addr → caller       │
     0x7ffe050   │ ?? (padding)               │
     0x7ffe048   │ temp = 0x4052a0            │ ◄── %rdi ──────► heap int @ 0x4052a0
     0x7ffe040   │ ?? (padding)               │ ◄── %rsp
```

Now the temporary exists in memory and owns the heap `int`.

> **Why are `leaq` and this `movq` "backwards"?** In source order you'd build the
> temporary first and then take its address. `leaq` doesn't depend on `%rax`,
> though, so GCC scheduled it earlier. The final state is the same either way.
> The comments in the listing above describe the logical order across the two
> lines.

#### `call take_up`: push a return address and jump

```
     0x7ffe058   │ return addr → caller       │
     0x7ffe050   │ ?? (padding)               │
     0x7ffe048   │ temp = 0x4052a0            │ ◄── %rdi   (take_up's parameter "p")
     0x7ffe040   │ ??                         │
                 ├────────────────────────────┤
     0x7ffe038   │ return addr → movq below   │ ◄── %rsp   (pushed by call)
                 │   ... take_up's frame ...  │
                 ▼
```

`take_up`'s parameter lives in `pass_up`'s frame, not in its own. `take_up` works
on it through `%rdi`, and one of two things happens:

```
                         take_up(temp @ 0x7ffe048)
                          ┌──────────┴──────────┐
               (A) only uses it          (B) moves from it
                    e.g. reads *p             e.g. stores it somewhere
                          │                        │
              temp = 0x4052a0 (unchanged)   temp = 0   (take_up wrote nullptr
                                                       into pass_up's slot)
```

When `take_up` returns, `ret` pops `0x7ffe038`, so `%rsp` is back at
`0x7ffe040`.

This is why it **can't be a tail call** (`jmp`), unlike `pass_raw`. `pass_up`
still owes a destructor call on `temp`, and a `jmp` would tear down the frame
that `temp` lives in while `take_up` is still using it.

#### `movq 8(%rsp), %rdi`: start `~unique_ptr()` on the temporary

```
     0x7ffe048   │ temp = 0x4052a0  or  0     │
     0x7ffe040   │ ??                         │ ◄── %rsp

   %rdi = whatever temp holds now   (also sets up argument 1 for operator delete)
```

`pass_up` can't know which case happened, because `take_up` is in another
translation unit. So it has to reload the value from memory.

#### `testq %rdi, %rdi` / `je .L3`: null check

```
                      %rdi == 0 ?
                  ┌───────┴────────┐
                yes (B)           no (A)
                  │                │
            jump to .L3        fall through
         (nothing to free)     (temp still owns the int)
```

`testq %rdi, %rdi` ANDs the register with itself and sets ZF if the result is
zero. This is the `if (ptr) delete ptr;` inside `~unique_ptr`.

#### `movl $4, %esi`: argument 2 is the size

```
   %rdi = 0x4052a0     (void* ptr)
   %rsi = 4            (sizeof(int); writing %esi zero-extends into all of %rsi)
```

GCC calls the **sized** `operator delete(void*, std::size_t)` because it knows
the static type is `int`.

#### `call operator delete(void*, unsigned long)`

```
     0x7ffe048   │ temp = 0x4052a0 (dangling) │
     0x7ffe040   │ ??                         │
                 ├────────────────────────────┤
     0x7ffe038   │ return addr → .L3          │ ◄── %rsp   (pushed by call)
                                                           heap int @ 0x4052a0 → FREED
```

#### `.L3`: `addq $24, %rsp` / `ret`

`addq` drops the 24-byte frame, putting `%rsp` back at `0x7ffe058`, and `ret`
pops the return address to go back to the caller. Both paths (A) and (B) end
here.

#### Who owns the heap `int` at each step

```
step:          entry  load   null   leaq   store  call take_up          dtor
               ─────  ────   ────   ────   ─────  ────────────────     ────
caller's p     OWNS   OWNS   0      0      0      0                    0
%rax           ─      copy   OWNS   OWNS   copy   (clobbered)          ─
temp @8(%rsp)  ─      ??     ??     ??     OWNS   OWNS (A) │ 0 (B)     (A) → delete
                                                   │  or   │           (B) → skip
take_up        ─      ─      ─      ─      ─      borrows  │ OWNS
```

#### Side by side with `pass_raw`

```
pass_raw:                          pass_up:
  %rdi = int*  ──jmp──► take_raw     %rdi = &p ──► load, null p, spill to stack,
  (0 stack bytes, 1 instr)           %rdi = &temp ──call──► take_up
                                     ◄── reload, test, maybe delete, pop, ret
                                     (24 stack bytes, 13 instrs, 2 memory writes
                                      before the call, 1 reload after)
```

The two versions do the same logical thing: hand an `int*` to another function.
In `pass_up`, every extra instruction comes from the ABI rule that a type with a
non-trivial destructor or move constructor must be passed through memory, and
that the caller must destroy it.

### Compared with a raw pointer

1. **The value has to go through memory.** The caller must build the temporary
   in its own stack frame, because only an object in memory has an address to
   pass. A value that could have stayed in a register gets written to the stack.
2. **The caller can't tail-call.** It still has to destroy the temporary after
   the call returns, so `jmp` becomes `call` plus cleanup.
3. **Every call site gets a destructor.** The callee is usually in another
   translation unit, so the caller can't tell whether it moved from the
   parameter. Every call site therefore gets a null check and an
   `operator delete` call. With exceptions on, there is also a landing pad (a
   `.cold` clone) that runs the same destructor if `take_up` throws.
4. **The callee does an extra load.** `read_up` has to load the `unique_ptr`
   before it can load the `int`.
5. **Ownership works the wrong way round.** Under Itanium the caller destroys
   the parameter, not the callee. If the callee takes ownership, it has to write
   null back into the caller's temporary so that the caller's destructor does
   nothing. And if the callee doesn't move from it, the `int` is freed in the
   caller when the call's full-expression ends, not when the callee returns.

### Why it can't just be fixed

This is part of the ABI, not a missed optimization. If `unique_ptr` were passed
in a register, every existing binary compiled with the current convention would
be incompatible with new code. Chandler Carruth used this example in his CppCon
2019 talk "There Are No Zero-cost Abstractions."

Clang has an escape hatch, `[[clang::trivial_abi]]`, which passes the type in
registers and makes the callee destroy it. libc++ offers an opt-in ABI flag
(`_LIBCPP_ABI_ENABLE_UNIQUE_PTR_TRIVIAL_ABI`) that applies it to `unique_ptr`.
libstdc++, which gsdb uses with GCC, has no such option.

### In practice

Taking `unique_ptr` by value is still the right way to say "this function takes
ownership" (Core Guidelines R.32). The cost is a handful of instructions and one
trip through memory per call, which only matters in hot code. The mistake is
using it where ownership doesn't change hands. If the function only uses the
object, take `T*` or `T&` (R.30).

For gsdb, if the expression evaluator calls a function with a by-value
`unique_ptr` parameter, it has to do what `pass_up` does: put the 8 bytes in
inferior memory, pass their address in the next integer register, and deal with
the destructor afterwards.
