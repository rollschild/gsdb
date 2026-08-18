# Why `offset >> 1`? — Register Offset Canonicalization

> **Subject:** `src/registers.cpp:133-145` — `registers::undefine()` / `registers::is_undefined()`
>
> ```cpp
> // shift the register offset 1 bit to the right so that registers at a byte
> // offset like `hl` are considered the same as their containing register
> std::size_t canonical_offset = register_info_by_id(id).offset >> 1;
> ```
>
> **Related docs:** [`register-xmacro-explained.md`](register-xmacro-explained.md) ·
> [`x86-64-sub-registers.md`](x86-64-sub-registers.md) · [`x86-64-rip-rsp-rbp.md`](x86-64-rip-rsp-rbp.md)
>
> **Assembly in this doc is AT&T syntax** (`mnemonic src, dst`), matching
> `ZydisDisassembleATT` in `src/disassembler.cpp:31` and the `.s` targets in `test/targets/`.

---

## TL;DR

`undefined_` tracks which registers DWARF CFI has declared unrecoverable. It is keyed on the
register's **byte offset inside `struct user`**, because offset is a natural identity for
*storage location* — `%rax`, `%eax`, `%ax`, and `%al` all literally have offset `80`, so keying on
offset unifies them for free.

The one exception is the high-byte registers. `%ah` is bits 8–15 of `%rax`, so `DEFINE_GPR_8H`
records it at offset `81` — one past its parent. `>> 1` discards that low bit, collapsing
`{80, 81}` to the single key `40`, while leaving genuinely different registers apart
(`%rcx` at `88` → `44`).

---

## Contents

1. [The hardware: what `%ah` actually is](#1-the-hardware-what-ah-actually-is)
2. [The encoding: how `registers.inc` describes it](#2-the-encoding-how-registersinc-describes-it)
3. [The numbers: real offsets on this machine](#3-the-numbers-real-offsets-on-this-machine)
4. [The problem: what `undefined_` needs](#4-the-problem-what-undefined_-needs)
5. [The fix: why `>> 1` works](#5-the-fix-why--1-works)
6. [The proof: verified against the real table](#6-the-proof-verified-against-the-real-table)
7. [Observations](#7-observations)

---

## 1. The hardware: what `%ah` actually is

There are only **16** general-purpose registers on x86-64. `%eax`, `%ax`, `%al`, and `%ah` are not
additional registers — they are *names for sub-ranges of the bits of `%rax`*. One piece of storage,
five names.

### Bit layout

```
 63                           32 31           16 15    8 7     0
┌───────────────────────────────┬───────────────┬───────┬───────┐
│                               │               │  %ah  │  %al  │
│                               │               ├───────┴───────┤
│                               │               │      %ax      │
│                               ├───────────────┴───────────────┤
│                               │              %eax             │
├───────────────────────────────┴───────────────────────────────┤
│                              %rax                             │
└───────────────────────────────────────────────────────────────┘
```

Each generation widened the same architectural register and kept the old name for the low half:

| Era             | Width | Name        | Covers                        |
| --------------- | ----- | ----------- | ----------------------------- |
| 8086 (1978)     | 8     | `%al`/`%ah` | bits 0–7 / bits **8–15**      |
| 8086            | 16    | `%ax`       | bits 0–15 (`%ah:%al`)         |
| 80386 (1985)    | 32    | `%eax`      | bits 0–31 (**E**xtended)      |
| x86-64 (2003)   | 64    | `%rax`      | bits 0–63 (**R**egister)      |

Note the asymmetry that drives everything below: **`%al`, `%ax`, `%eax`, and `%rax` are all anchored
at bit 0. `%ah` is not** — it starts at bit 8.

### Byte layout inside `struct user`

`registers::data_` is a whole `struct user`, addressed by byte offset. On a little-endian machine
bit 0 lives in the *lowest-addressed* byte, so the bit picture above becomes:

```
                   +0     +1     +2     +3     +4     +5     +6     +7
                ┌──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┐
byte in `user`  │  80  │  81  │  82  │  83  │  84  │  85  │  86  │  87  │
                └──────┴──────┴──────┴──────┴──────┴──────┴──────┴──────┘
%al  (size 1)   ├──────┤                                                    offset 80
%ah  (size 1)          ├──────┤                                             offset 81  ◄── odd!
%ax  (size 2)   ├─────────────┤                                             offset 80
%eax (size 4)   ├───────────────────────────┤                               offset 80
%rax (size 8)   ├───────────────────────────────────────────────────────┤   offset 80
```

Four of the five names land on byte 80. Only `%ah` lands on 81.

### Which names exist for which register

The high-byte names are a leftover of the original four 8086 registers:

| 64-bit                  | 32-bit          | 16-bit         | low 8                  | high 8                    |
| ----------------------- | --------------- | -------------- | ---------------------- | ------------------------- |
| `%rax %rbx %rcx %rdx`   | `%eax` …        | `%ax` …        | `%al %bl %cl %dl`      | **`%ah %bh %ch %dh`**     |
| `%rsi %rdi %rbp %rsp`   | `%esi` …        | `%si` …        | `%sil %dil %bpl %spl`  | *(none)*                  |
| `%r8`–`%r15`            | `%r8d`–`%r15d`  | `%r8w`–`%r15w` | `%r8b`–`%r15b`         | *(none)*                  |

That is exactly the shape of `registers.inc:73-83` — **four** `DEFINE_GPR_8H` lines, **sixteen**
`DEFINE_GPR_8L` lines.

> **Encoding aside.** `%sil`/`%dil`/`%bpl`/`%spl` are new in x86-64 and only reachable *with* a REX
> prefix; *with* a REX prefix the four high-byte registers become unencodable. So no single
> instruction can touch `%ah` and `%r8b` at once.

### Write semantics (the CPU vs. the debugger)

When the **CPU** writes a sub-register, width matters:

```
movl $1, %eax     # zero-extends: %rax becomes 0x0000000000000001
movb $1, %al      # merges: bits 8-63 of %rax untouched
movb $1, %ah      # merges into bits 8-15
xorl %eax, %eax   # idiomatic way to zero all 64 bits of %rax
```

`gsdb::registers::write` (`src/registers.cpp:96`) does **not** model the zero-extension rule — it
copies `info.size` bytes at `info.offset`, so `register write eax 1` preserves the upper 32 bits.
That matches GDB's behavior for `$eax` and is the right call for a debugger: you are editing a
bit-field of saved state, not executing a `movl`.

---

## 2. The encoding: how `registers.inc` describes it

### The X-macro mechanism

`registers.inc` is not a header you include for declarations. It is a **data file written as macro
calls**, meant to be pasted into a context that has already defined `DEFINE_REGISTER`. Hence the
guard at `registers.inc:1-4`:

```c
#ifndef DEFINE_REGISTER
#error "This file is intended for textual inclusion with the DEFINE_REGISTER macro defined"
#endif
```

`register_info.hpp` includes the same file **twice**, with two different definitions:

```
                        registers.inc
              ┌──────────────────────────────────┐
              │  DEFINE_GPR_64(rax, 0),          │   the data — one line
              │  DEFINE_GPR_32(eax, rax),        │   per register, with a
              │  DEFINE_GPR_8H(ah,  rax),        │   trailing comma so the
              │  ...                             │   text is a valid list
              └──────────────────────────────────┘
                               │
                #include'd twice, with different
                   definitions of DEFINE_REGISTER
                               │
              ┌────────────────┴────────────────┐
              ▼                                 ▼
  register_info.hpp:15                register_info.hpp:51
  #define DEFINE_REGISTER(...) name   #define DEFINE_REGISTER(...) {...}
  (keep only the name)                (keep every column)
              │                                 │
              ▼                                 ▼
  enum class register_id {            inline constexpr register_info
      rax, rdx, rcx, ...,             g_register_infos[] = {
      eax, ..., ah, al, ...               {register_id::rax, "rax", 0, 8, 80, ...},
  };                                       ...
                                      };
```

`#name` is the preprocessor's stringify operator, turning the token `rax` into `"rax"`. The payoff:
the enum and the table **cannot drift out of sync**, and adding a register is a one-liner.

### `GPR_OFFSET` — a two-hop `offsetof`

```c
#define GPR_OFFSET(reg) (offsetof(user, regs) + offsetof(user_regs_struct, reg))
```

`ptrace`'s `PTRACE_PEEKUSER`/`POKEUSER` address `struct user` by byte offset, so every
`register_info::offset` is measured **from the start of `struct user`**. Getting there takes two
hops:

```
   struct user
   ┌─────────────────────────────────────────────────┐
   │ regs : user_regs_struct                         │  offsetof(user, regs) == 0
   │   ┌─────────────────────────────────────────┐   │
   │   │ ... r15, r14, r13, r12, rbp, rbx, ...   │   │
   │   │              rax  ◄─────────────────────┼───┼── offsetof(user_regs_struct, rax) == 80
   │   └─────────────────────────────────────────┘   │
   │ i387 : user_fpregs_struct                       │
   │ u_debugreg[8]                                   │
   └─────────────────────────────────────────────────┘
                  GPR_OFFSET(rax)  =  0 + 80  =  80
```

### The five `DEFINE_GPR_*` macros

Each is shorthand that fills in the boilerplate columns of
`DEFINE_REGISTER(name, dwarf_id, size, offset, type, format)`:

| Macro                            | 2nd param   | `dwarf_id`   | `size` | `offset`                  | `type`    |
| -------------------------------- | ----------- | ------------ | ------ | ------------------------- | --------- |
| `DEFINE_GPR_64(name, dwarf_id)`  | DWARF id    | as given     | 8      | `GPR_OFFSET(name)`        | `gpr`     |
| `DEFINE_GPR_32(name, super)`     | parent reg  | `-1`         | 4      | `GPR_OFFSET(super)`       | `sub_gpr` |
| `DEFINE_GPR_16(name, super)`     | parent reg  | `-1`         | 2      | `GPR_OFFSET(super)`       | `sub_gpr` |
| `DEFINE_GPR_8L(name, super)`     | parent reg  | `-1`         | 1      | `GPR_OFFSET(super)`       | `sub_gpr` |
| **`DEFINE_GPR_8H(name, super)`** | parent reg  | `-1`         | 1      | **`GPR_OFFSET(super)+1`** | `sub_gpr` |

Two details worth calling out:

- **Why the `super` parameter exists.** `user_regs_struct` has a field named `rax` but **no field
  named `eax`, `ax`, `al`, or `ah`** — those aren't storage, they're views. The offset must be
  computed from the field that does exist. `DEFINE_GPR_64` needs no `super` because its name *is*
  the field name.
- **Why `dwarf_id` is `-1` for sub-registers.** The x86-64 psABI's DWARF register numbering only
  assigns numbers to the 16 full-width registers; CFI rules and location expressions never talk
  about `%ah`. `-1` is the "no DWARF number" sentinel, so `register_info_by_dwarf()` simply never
  matches those entries.

### Two expansions, all the way down

```
DEFINE_GPR_64(rax, 0)
  ├─► DEFINE_REGISTER(rax, 0, 8, (offsetof(user,regs)+offsetof(user_regs_struct,rax)),
  │                   register_type::gpr, register_format::uint)
  └─► {register_id::rax, "rax",  0, 8, 80, register_type::gpr,     register_format::uint}
                                        ▲
                                        └── even

DEFINE_GPR_8H(ah, rax)
  ├─► DEFINE_REGISTER(ah, -1, 1, (offsetof(user,regs)+offsetof(user_regs_struct,rax)) + 1,
  │                   register_type::sub_gpr, register_format::uint)
  └─► {register_id::ah,  "ah",  -1, 1, 81, register_type::sub_gpr, register_format::uint}
                                        ▲
                                        └── ODD — this is the entire origin of the `>> 1`
```

---

## 3. The numbers: real offsets on this machine

Measured via the actual `offsetof` chain:

```
%rax  %eax  %ax  %al ──►  80        %ah ──►  81
%rbx  %ebx  %bx  %bl ──►  40        %bh ──►  41
%rcx  %ecx  %cx  %cl ──►  88        %ch ──►  89
%rdx  %edx  %dx  %dl ──►  96        %dh ──►  97
%rsi  %esi  %si  %sil ─► 104        (no high-byte name)
%rdi  %edi  %di  %dil ─► 112        (no high-byte name)
```

So `offset` already does most of the work: **`%rax`, `%eax`, `%ax`, and `%al` are literally the
number 80** — indistinguishable, which is exactly what we want. Only `%ah` differs, and only by 1.

Two questions hide in that sentence: *why 80*, and *why do four names share it*. They have entirely
different answers.

### Why 80 — kernel struct layout

`user_regs_struct` is a plain C struct of 27 `unsigned long long` fields, laid out in the kernel's
register-save order. `rax` is simply the 11th field:

```
   off  field          off  field
   ───  ─────          ───  ─────
     0  r15             80  rax        ◄── 11th field: 10 × 8 = 80
     8  r14             88  rcx
    16  r13             96  rdx
    24  r12            104  rsi
    32  rbp            112  rdi
    40  rbx            120  orig_rax
    48  r11            128  rip
    56  r10            136  cs
    64  r9             144  eflags
    72  r8             152  rsp        ◄── 152 >> 1 = 76, the key seen in §6
```

There is nothing meaningful about 80 — it means "ten 8-byte fields came before me." The order is
not alphabetical, not numeric, and not DWARF order; it is the order the kernel pushes registers on
entry (`struct pt_regs`). That is why `%rbx` lands at 40 while `%rsp` sits way out at 152.

`GPR_OFFSET` then adds `offsetof(user, regs)`, which is `0` because `regs` is the first member of
`struct user` — so on this platform the offset *within* `user_regs_struct` is also the offset within
`user`.

### Why four names share it — little-endianness

Because `offset` is a **start address**, and `size` is what distinguishes the views:

```
                  +0     +1     +2     +3     +4     +5     +6     +7
                ┌──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┐
byte in `user`  │  80  │  81  │  82  │  83  │  84  │  85  │  86  │  87  │
                │bits  │bits  │bits  │bits  │bits  │bits  │bits  │bits  │
                │ 0-7  │ 8-15 │16-23 │24-31 │32-39 │40-47 │48-55 │56-63 │
                └──────┴──────┴──────┴──────┴──────┴──────┴──────┴──────┘
                    ▲
                    └── little-endian: bit 0 lives at the LOWEST address
```

`%al` = bits 0–7, `%ax` = bits 0–15, `%eax` = bits 0–31, `%rax` = bits 0–63. All four are anchored
at bit 0, so all four *start* at byte 80; they differ only in how far right they extend. The
identity of a register view is therefore the **pair** `(offset, size)`, and `register_info` stores
both:

| name    | offset | size | bytes read |
| ------- | ------ | ---- | ---------- |
| `%rax`  | 80     | 8    | 80–87      |
| `%eax`  | 80     | 4    | 80–83      |
| `%ax`   | 80     | 2    | 80–81      |
| `%al`   | 80     | 1    | 80         |
| `%ah`   | **81** | 1    | 81         |

Which is exactly what `registers::read` does — same base pointer, different width
(`src/registers.cpp:53-65`):

```cpp
case 1: return from_bytes<std::uint8_t> (bytes + info.offset);
case 2: return from_bytes<std::uint16_t>(bytes + info.offset);
case 4: return from_bytes<std::uint32_t>(bytes + info.offset);
case 8: return from_bytes<std::uint64_t>(bytes + info.offset);
```

The matching numbers are not a coincidence, either: `DEFINE_GPR_32`, `_16`, and `_8L` all pass the
*same expression*, `GPR_OFFSET(super)` (`registers.inc:13, 17, 25`). Only `_8H` adds `+ 1`, because
`%ah` is the one view not anchored at bit 0.

### The counterfactual

The shared offset is a *consequence* of endianness, not a definition. On a hypothetical big-endian
x86 the low byte would sit at the highest address, and every alias would start somewhere different:

```
   little-endian (real x86)           big-endian (hypothetical)
   byte:  80 81 82 83 84 85 86 87     byte:  80 81 82 83 84 85 86 87
   %al:   ▓▓                          %al:                        ▓▓   offset 87
   %ax:   ▓▓ ▓▓                       %ax:                     ▓▓ ▓▓   offset 86
   %eax:  ▓▓ ▓▓ ▓▓ ▓▓                 %eax:              ▓▓ ▓▓ ▓▓ ▓▓   offset 84
   %rax:  ▓▓ ▓▓ ▓▓ ▓▓ ▓▓ ▓▓ ▓▓ ▓▓     %rax:  ▓▓ ▓▓ ▓▓ ▓▓ ▓▓ ▓▓ ▓▓ ▓▓   offset 80
          ▲                                  ▲
          all share offset 80                four different offsets
```

The whole `offset`-as-key design in §4 depends on the left column. On a big-endian target, offset
would *separate* the aliases instead of unifying them, and no amount of `>> 1` would fix it.

---

## 4. The problem: what `undefined_` needs

```cpp
std::vector<std::size_t> undefined_;   // include/libgsdb/registers.hpp:60
```

DWARF call-frame information can declare that a register has **no recoverable value** in a given
frame — the `DW_CFA_undefined` rule (`detail/dwarf.h:529`, modeled as `undefined_rule` in
`src/dwarf.cpp:636`). When the unwinder reconstructs a caller's frame it calls `undefine(id)`
(`src/dwarf.cpp:828`), and
any later `read()` of that register throws (`src/registers.cpp:47-49`).

The design question is **what to key that set on.**

### ✗ Keying on `register_id` — broken

`register_id` is per-*name*. Five distinct ids (`rax`, `eax`, `ax`, `al`, `ah`) name one physical
register, so `undefine(rax)` would leave `read(al)` happily returning garbage from a register the
unwinder just declared unrecoverable. You'd need an explicit name→parent mapping table.

### ✓ Keying on `offset` — almost perfect

Offset *is* the identity of a storage location. Two names with the same offset name the same bits,
so the aliasing comes for free — no mapping table required. That's the clever part of the design.

But raw offset gets **4 of the 5 right**:

```
   undefine(%rax)  ──► push 80 ──► undefined_ = [80]

   is_undefined(%eax) ──► 80 ──► ✓ hit
   is_undefined(%ax)  ──► 80 ──► ✓ hit
   is_undefined(%al)  ──► 80 ──► ✓ hit
   is_undefined(%ah)  ──► 81 ──► ✗ MISS  ← read succeeds on an unrecoverable register
```

---

## 5. The fix: why `>> 1` works

We need a function `f(offset)` satisfying two constraints:

|   | Requirement          | Meaning                                        |
| - | -------------------- | ---------------------------------------------- |
| 1 | `f(80) == f(81)`     | merge a high-byte register into its parent     |
| 2 | `f(80) != f(88)`     | but keep `%rax` and `%rcx` apart               |

`>> 1` is integer division by 2 — it **discards the low bit**:

```
                 offset in `struct user`                      >> 1        key
                 ═══════════════════════                                  ═══
   %rbx %ebx %bx %bl ──── 40 ──┐
   %bh  ───────────────── 41 ──┴──────────────────────────────────────►    20
   %rax %eax %ax %al ──── 80 ──┐
   %ah  ───────────────── 81 ──┴──────────────────────────────────────►    40
   %rcx %ecx %cx %cl ──── 88 ──┐
   %ch  ───────────────── 89 ──┴──────────────────────────────────────►    44
   %rdx %edx %dx %dl ──── 96 ──┐
   %dh  ───────────────── 97 ──┴──────────────────────────────────────►    48
   %rsi %esi %si %sil ── 104 ─────────────────────────────────────────►    52
   %rdi %edi %di %dil ── 112 ─────────────────────────────────────────►    56
                                 ▲                                          ▲
                    pairs differing by 1 collapse           distinct registers stay
                                                            ≥ 8 apart → ≥ 4 apart
```

### End-to-end

```
   CFI: DW_CFA_undefined for DWARF register 0  (%rax)
                        │
                        ▼
            undefine(register_id::rax)
                        │
        offset 80  ──── >> 1 ────►  40  ──► undefined_ = [40]


   later:  read(register_id::ah)          ← a *different* register_id
                        │
        offset 81  ──── >> 1 ────►  40  ──► found in undefined_
                                             │
                                             ▼
                              error::send("Register is undefined")
```

---

## 6. The proof: verified against the real table

The argument rests on two structural facts:

- **(a) Every real storage slot starts at an even offset.** The GPR fields of `user_regs_struct` are
  `unsigned long long`, so every `GPR_OFFSET(x)` is a multiple of 8. Since `base` is even,
  `(base + 1) >> 1 == base >> 1` — the `+1` falls off the bottom.
- **(b) The only odd offsets in the whole table come from `DEFINE_GPR_8H`.** Nothing else in
  `registers.inc` ever produces an odd number.

Together: two offsets collide under `>> 1` **iff** they differ by exactly 1 with the smaller being
even — which by (a)+(b) happens *only* for the four `{base, base+1}` pairs. That is precisely the
merge we want.

Rather than take the argument on faith, iterate `g_register_infos[]` directly:

```
total register entries : 125
distinct offsets       :  69
distinct keys (>>1)    :  65      ◄── exactly 4 fewer
odd offsets in table   :   4      ◄── bh(41), ah(81), ch(89), dh(97) — and nothing else
```

**69 − 65 = 4**, matching the four high-byte registers one-for-one. Every group the shift merges is
a legitimate alias group:

```
   key    merged names
   ───    ─────────────────────────────
     0    r15  r15d  r15w  r15b
     4    r14  r14d  r14w  r14b
     8    r13  r13d  r13w  r13b
    12    r12  r12d  r12w  r12b
    16    rbp  ebp   bp    bpl
    20    rbx  ebx   bx    bh    bl     ◄── high byte folded in
    24    r11  r11d  r11w  r11b
    28    r10  r10d  r10w  r10b
    32    r9   r9d   r9w   r9b
    36    r8   r8d   r8w   r8b
    40    rax  eax   ax    ah    al     ◄── high byte folded in
    44    rcx  ecx   cx    ch    cl     ◄── high byte folded in
    48    rdx  edx   dx    dh    dl     ◄── high byte folded in
    52    rsi  esi   si    sil
    56    rdi  edi   di    dil
    76    rsp  esp   sp    spl
   128    st0  mm0                      ┐
   136    st1  mm1                      │  x87 / MMX genuinely share
   ...                                  │  storage — merging is correct
   184    st7  mm7                      ┘
```

No spurious merges. The FPR control block survives too, even though its fields are only 2 bytes
apart (`fcw`/`fsw`/`ftw`/`fop` at +0/+2/+4/+6 inside `i387`): a gap of 2 becomes a gap of 1 —
squeezed, but still distinct.

---

## 7. Observations

### The shift is doing *rounding*, not compression

The operation actually needed is "round an odd offset down to the even one below it." `>> 1`
achieves that as a side effect of dropping bit 0; the halving is incidental.

```
   offset >> 1              offset & ~std::size_t{1}
   ───────────              ────────────────────────
   80 ──► 40                80 ──► 80
   81 ──► 40                81 ──► 80
   88 ──► 44                88 ──► 88
        ▲                         ▲
   an opaque key —          still a real byte offset,
   40 is not an offset      so `canonical_offset` is
   of anything              an honest name
```

Both are correct. The mask expresses the rule directly and keeps the value meaningful, which is why
the shift reads as a puzzle on first encounter. (Compare `src/registers.cpp:126`, which uses
`info.offset & ~0b111` for the unrelated `POKEUSER` 8-byte-alignment requirement.) Since nothing
outside these two functions ever reads the key, the choice is cosmetic.

### `undefine()` does not deduplicate

It is a `push_back` paired with a linear `std::find`. Calling `undefine(rax)` then `undefine(al)`
stores `40` twice. With ~125 register entries and a fresh `registers` object per frame this is a
non-issue; it would only matter if one `registers` instance were reused across many unwind steps.

### The comment's `` `hl` `` is a slip (half-fixed)

`src/registers.cpp:141`, in `is_undefined()`, still says *"registers at a byte offset like `hl`"*.
There is no `hl` register on x86-64 (that's a Z80/8080 register pair). The case actually handled is
the **high-byte** family — `%ah`, `%bh`, `%ch`, `%dh` — since the `l` variants already share the
parent's offset and need no fixing. The twin comment in `undefine()` (`src/registers.cpp:135`) has
since been corrected to say `ah`; only the `is_undefined()` copy still carries the typo.
