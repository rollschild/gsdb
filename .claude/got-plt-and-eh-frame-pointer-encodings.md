# `.got.plt`, the GOT/PLT machinery, and why `dwarf.cpp` needs its address

Reference doc for `src/dwarf.cpp:673-676`:

```cpp
auto text_section_start = *elf.get_section_start_address(".text");
// file address of the start of the `.got.plt` section
auto plt_start =
    elf.get_section_start_address(".got.plt").value_or(gsdb::file_addr{});
```

All addresses/dumps below are real output from binaries on this machine; the exact
commands to reproduce them are at the end.

---

## TL;DR

`.got.plt` is the writable jump-slot table that the dynamic linker patches so that
calls to functions in shared libraries reach the right place. It has **nothing to do
with DWARF** — `dwarf.cpp` only wants its *start address* because it doubles as the
GOT base, and the "GOT base" is one of the five bases that a `DW_EH_PE_*`-encoded
pointer inside `.eh_frame` can be relative to (`DW_EH_PE_datarel`). The two locals in
`execute_cfi_instruction` are simply the two "base address" operands that get handed
to `parse_eh_frame_pointer()` when a CFI instruction (`DW_CFA_set_loc`) carries an
encoded address.

---

# Part 1 — What `.got.plt` actually is

## 1.1 The problem it solves

When your program calls `puts`, the compiler emits a `callq` — but at compile time
nobody knows where `puts` will live. `libc.so` is mapped at an address chosen at
runtime (ASLR), and the same `libc.so` is shared, read-only, between every process on
the machine.

Two constraints collide:

1. **Code must not be patched at load time.** If the loader rewrote the `callq` target
   inside `.text`, every process would need its own private, dirtied copy of every
   code page — sharing dies, memory usage explodes. So `.text` stays read-only and
   position-independent.
2. **The target address is only known at runtime.**

The resolution is the classic one: add a level of indirection through a *writable data
table*. Code stays fixed and shareable; only the data table is per-process and gets
patched.

That table is the **GOT** (Global Offset Table). The stubs that jump through it for
*function* calls are the **PLT** (Procedure Linkage Table).

## 1.2 GOT vs PLT vs `.got` vs `.got.plt`

| Section    | Flags | Holds |
|------------|-------|-------|
| `.plt`     | `AX` (read+exec) | Executable stubs, 16 bytes each — one per called external function |
| `.got`     | `WA` (read+write) | Slots for **data** symbols and eagerly-bound function symbols (`R_X86_64_GLOB_DAT`) |
| `.got.plt` | `WA` (read+write) | Slots for **lazily-bound function** symbols (`R_X86_64_JUMP_SLOT`), plus 3 reserved entries |

The split exists purely for **RELRO** (see §1.5): `.got` can be made read-only right
after the loader finishes relocating, but `.got.plt` must stay writable if lazy
binding is in use, since the resolver writes into it on the first call to each
function. Keeping them in separate sections lets the loader `mprotect` one and not
the other.

`.plt.got` (note the reversed name — a different section!) holds PLT-style stubs for
symbols that were bound eagerly and therefore live in `.got` rather than `.got.plt`.

## 1.3 Concrete layout

Build a binary that actually uses lazy binding:

```
gcc -O0 -no-pie -Wl,-z,lazy,-z,norelro pltdemo.c -o pltdemo
```

Sections:

```
  [ 9] .rela.plt   RELA      0000000000400560  size 0x30
  [11] .plt        PROGBITS  0000000000401020  size 0x30   AX
  [12] .text       PROGBITS  0000000000401050  size 0x119  AX
  [21] .dynamic    DYNAMIC   0000000000403138  size 0x1f0  WA
  [22] .got        PROGBITS  0000000000403328  size 0x10   WA
  [23] .got.plt    PROGBITS  0000000000403338  size 0x28   WA
```

And the symbol that names the GOT base:

```
    17: 0000000000403338     0 OBJECT  LOCAL  DEFAULT   23 _GLOBAL_OFFSET_TABLE_
```

`_GLOBAL_OFFSET_TABLE_ == 0x403338 == start of .got.plt`. **This equality is the
entire reason `dwarf.cpp` asks for `.got.plt`'s address** — see Part 2.

The 40 bytes of `.got.plt` (five 8-byte slots), little-endian:

```
 403338 38314000 00000000   GOT[0] = 0x403138  -> address of .dynamic
 403340 00000000 00000000   GOT[1] = 0         -> filled at runtime: link_map*
 403348 00000000 00000000   GOT[2] = 0         -> filled at runtime: _dl_runtime_resolve
 403350 36104000 00000000   GOT[3] = 0x401036  -> puts@plt   + 6
 403358 46104000 00000000   GOT[4] = 0x401046  -> printf@plt + 6
```

Note GOT[0]: `0x403138` is exactly the address of `.dynamic` in the table above. The
three reserved slots are mandated by the AMD64 psABI.

And the PLT stubs:

```asm
# .plt at 0x401020 — PLT0, the "resolver trampoline", 1 per binary
0000000000401020 <PLT0>:
  401020:  pushq  0x231a(%rip)        # 0x401026 + 0x231a = 0x403340 = GOT[1]
  401026:  jmpq   *0x231c(%rip)       # 0x40102c + 0x231c = 0x403348 = GOT[2]
  40102c:  nopl   0x0(%rax)

0000000000401030 <puts@plt>:
  401030:  jmpq   *0x231a(%rip)       # 0x401036 + 0x231a = 0x403350 = GOT[3]
  401036:  pushq  $0x0                #   reloc index 0
  40103b:  jmpq   401020 <PLT0>

0000000000401040 <printf@plt>:
  401040:  jmpq   *0x2312(%rip)       # 0x401046 + 0x2312 = 0x403358 = GOT[4]
  401046:  pushq  $0x1                #   reloc index 1
  40104b:  jmpq   401020 <PLT0>
```

Every `%rip`-relative displacement resolves to a `.got.plt` slot. That's the
indirection: `.text` contains `callq puts@plt`, a fixed link-time-known address, and
the mutable part lives entirely in data.

The relocations that describe those slots:

```
Relocation section '.rela.plt' contains 2 entries:
    Offset             Type                Symbol
0000000000403350   R_X86_64_JUMP_SLOT      puts@GLIBC_2.2.5 + 0
0000000000403358   R_X86_64_JUMP_SLOT      printf@GLIBC_2.2.5 + 0
```

`R_X86_64_JUMP_SLOT` is the "this is a lazy PLT slot" relocation type, and the offsets
match GOT[3] and GOT[4] exactly.

## 1.4 The lazy-binding dance, step by step

**First ever call to `puts`:**

1. `.text` executes `callq 401030 <puts@plt>`.
2. `puts@plt` does `jmpq *0x231a(%rip)` — an *indirect* jump through GOT[3].
3. GOT[3] currently holds `0x401036`, which is the very next instruction in its own
   stub. So the jump is effectively a no-op fallthrough. This is the trick: the slot
   is pre-initialized to point *back into the stub* so the unresolved case costs
   nothing extra.
4. `pushq $0x0` — pushes the index of this symbol's entry in `.rela.plt`.
5. `jmpq 401020` — into PLT0.
6. PLT0 does `pushq 0x231a(%rip)` → pushes GOT[1], the `struct link_map*` for this
   object (the loader wrote it there at startup, which is why GOT[1] is zero in the
   file but non-zero at runtime).
7. PLT0 does `jmpq *0x231c(%rip)` → jumps through GOT[2] to `_dl_runtime_resolve`,
   which the loader also filled in at startup.
8. `_dl_runtime_resolve` now has everything it needs on the stack: which object
   (`link_map*`) and which symbol (reloc index). It performs the symbol lookup,
   **writes the real address of `puts` into GOT[3]**, and then jumps to it. Control
   never returns to the PLT stub.

**Every subsequent call to `puts`:**

1. `callq 401030 <puts@plt>`.
2. `jmpq *0x231a(%rip)` — GOT[3] now holds the real `puts`, so this is a single
   indirect jump straight into libc.

Steps 3–8 happen exactly once per symbol. Cost afterwards: one extra memory load.

The payoff is that a program linking 2000 libc symbols but calling 20 of them only
pays for 20 lookups. The cost is a writable, executable-reachable function-pointer
table — which is exactly why it's an attack target, which brings us to RELRO.

## 1.5 RELRO, `BIND_NOW`, and the disappearing section

An attacker with an arbitrary-write primitive who overwrites GOT[3] gets control flow
on the next `puts` call. Mitigations:

- **Partial RELRO** (`-z relro`): the loader `mprotect`s `.got` (and `.dynamic`,
  `.init_array`, …) read-only after relocating. `.got.plt` stays writable — lazy
  binding still works, but the GOT proper is protected.
- **Full RELRO** (`-z relro -z now`): `BIND_NOW` tells the loader to resolve *every*
  symbol eagerly at startup. Lazy binding is off, `_dl_runtime_resolve` is never
  needed, and the whole `.got.plt` region can be marked read-only — or elided
  entirely, with the jump slots merged into `.got` as `GLOB_DAT` relocations.

This is not hypothetical for this project. NixOS's GCC wrapper enables full RELRO by
default, so **this repo's own test targets have no `.got.plt` at all**:

```
$ readelf -SW build/test/targets/hello_gsdb | grep -E '\.got|\.plt|\.text'
  [11] .plt          PROGBITS  0000000000001020  size 0x20   AX
  [12] .plt.got      PROGBITS  0000000000001040  size 0x08   AX
  [13] .text         PROGBITS  0000000000001050  size 0x103  AX
  [23] .got          PROGBITS  0000000000003fb8  size 0x48   WA

$ readelf -dW build/test/targets/hello_gsdb | grep -i flags
 0x000000000000001e (FLAGS)    BIND_NOW
 0x000000006ffffffb (FLAGS_1)  Flags: NOW PIE

$ readelf -sW build/test/targets/hello_gsdb | grep GLOBAL_OFFSET_TABLE
    17: 0000000000003fb8  0 OBJECT LOCAL DEFAULT 23 _GLOBAL_OFFSET_TABLE_
```

No `.got.plt`. There *is* a `.plt.got` (the eager-binding stub section), which is a
different thing with a confusingly similar name. And critically:
`_GLOBAL_OFFSET_TABLE_ == 0x3fb8 == start of .got`, **not** `.got.plt`.

That is precisely why line 676 uses `.value_or(gsdb::file_addr{})` while line 673
dereferences the optional with `*`: `.text` is always there, `.got.plt` frequently
isn't. See §3.1 for the correctness caveat this raises.

---

# Part 2 — Why a DWARF parser cares

## 2.1 `.eh_frame` pointers are not plain addresses

`.eh_frame` (and its index `.eh_frame_hdr`) are `SHF_ALLOC` sections — unlike
`.debug_*`, they are *loaded into memory* and consumed at runtime by the C++ unwinder
during exception propagation. Because they're loaded, they participate in
position-independent code: storing an absolute 8-byte address in them would force a
runtime relocation per FDE, dirtying pages that could otherwise be shared.

So every pointer in `.eh_frame` is stored in a compressed, relocation-free form
described by a one-byte **encoding**. That byte splits into three parts:

```
 bit 7      bits 6-4        bits 3-0
 0x80       0x70            0x0f
 indirect   base            value format
```

**Value format (low nibble)** — how many bytes to read and how to interpret them.
Handled by `parse_eh_frame_pointer_with_base()` at `src/dwarf.cpp:201`:

| Constant | Value | Reads |
|---|---|---|
| `DW_EH_PE_absptr` | `0x00` | 8 bytes (native pointer size) |
| `DW_EH_PE_uleb128` | `0x01` | variable-length unsigned |
| `DW_EH_PE_udata2/4/8` | `0x02/03/04` | 2 / 4 / 8 bytes unsigned |
| `DW_EH_PE_sleb128` | `0x09` | variable-length signed |
| `DW_EH_PE_sdata2/4/8` | `0x0a/0b/0c` | 2 / 4 / 8 bytes **signed** |

**Base (bits 6–4)** — what the decoded value is added to. Handled by
`parse_eh_frame_pointer()` at `src/dwarf.cpp:230`:

| Constant | Value | Base is | Parameter in our code |
|---|---|---|---|
| `DW_EH_PE_absptr` | `0x00` | `0` — value is already absolute | — |
| `DW_EH_PE_pcrel` | `0x10` | the address of the pointer field itself | `pc` |
| `DW_EH_PE_textrel` | `0x20` | start of `.text` | `text_section_start` |
| `DW_EH_PE_datarel` | `0x30` | **the GOT base** | `data_section_start` |
| `DW_EH_PE_funcrel` | `0x40` | start of the function the FDE describes | `func_start` |
| `DW_EH_PE_aligned` | `0x50` | value is aligned to pointer size | *not handled* |

**Indirect (`0x80`)** — the decoded result is the address of a slot that *contains*
the real pointer; you must dereference once more. Deliberately skipped by our code,
hence the `encoding & 0x70` mask at `src/dwarf.cpp:239` and the comment above it.

So `sdata4|pcrel` (`0x1b`) means: read 4 bytes as a signed int, add the file address
of those 4 bytes. That's the overwhelmingly common encoding, and it's what the
compiler picked for our binaries — a 4-byte self-relative delta, no relocation
needed, works at any load address.

## 2.2 `DW_EH_PE_datarel` and the GOT

`datarel` says "relative to the beginning of the `.got` section". The AMD64 psABI
defines the GOT base as the value of the `_GLOBAL_OFFSET_TABLE_` symbol, and in the
classic lazy-binding layout the linker places that symbol at the start of `.got.plt`
— confirmed empirically in §1.3 (`_GLOBAL_OFFSET_TABLE_ == 0x403338 == .got.plt`).

Hence lines 675-676: `.got.plt`'s start address is being used as a stand-in for the GOT
base, so that if a `datarel`-encoded pointer shows up it can be resolved.

Why would the compiler ever choose `datarel` over the simpler `pcrel`? Because a
`datarel` value is *identical in every process* regardless of load address, and the
GOT register (`%r15`/`%rbx` in some ABIs, or just a known base) may already be
materialized — so a `datarel` pointer can be resolved without knowing where the
pointer itself lives. It shows up mostly in `.eh_frame_hdr` (see next section) and
occasionally for LSDA/personality-routine pointers.

## 2.3 The `.eh_frame_hdr` twist — `datarel` means something different there

Here's the wrinkle worth internalizing, because our code *already relies on it*.

The LSB spec defines `.eh_frame_hdr` separately from `.eh_frame`, and for the binary
search table inside `.eh_frame_hdr`, `DW_EH_PE_datarel` is relative to **the start of
`.eh_frame_hdr` itself**, not the GOT.

Real data from `hello_gsdb` (`.eh_frame_hdr` is at file address `0x2014`):

```
Contents of the .eh_frame_hdr section:
  Version:                 1
  Pointer Encoding Format: 0x1b (sdata4, pcrel)
  Count Encoding Format:   0x3  (udata4, absolute)
  Table Encoding Format:   0x3b (sdata4, datarel)     <-- 0x30 | 0x0b
  Entries in search table: 0x4
  0xfffffffffffff00c (offset: 0x1020) -> fde=[0x30]
  0xfffffffffffff02c (offset: 0x1040) -> fde=[0x58]
  0xfffffffffffff03c (offset: 0x1050) -> fde=[0x18]
  0xfffffffffffff125 (offset: 0x1139) -> fde=[0x70]
```

Decode the first entry by hand:

- Encoding `0x3b` → low nibble `0x0b` = `sdata4`, base bits `0x30` = `datarel`.
- Raw 4 bytes read as **signed** 32-bit: `0xfffff00c` = `-4084` = `-0xFF4`.
- Base = start of `.eh_frame_hdr` = `0x2014`.
- `0x2014 - 0xFF4 = 0x1020` ✓ — matches readelf's `(offset: 0x1020)`, which is
  `.plt`, the function that first FDE describes.

If we had used the GOT (`0x3fb8`) as the base here we'd have gotten `0x2fc4` —
garbage, pointing into `.dynamic`-ish data, not code.

Our implementation gets this right at `src/dwarf.cpp:1592-1596`, inside
`eh_hdr::operator[]`:

```cpp
auto eh_hdr_offset = elf->data_pointer_as_file_offset(start);
auto entry_address = parse_eh_frame_pointer(
    *elf, cur, encoding,
    current_offset.off() /* as the program counter value */,
    text_section_start.addr(), eh_hdr_offset.off(), 0);
                          //   ^^^^^^^^^^^^^^^^^^^^^^
                          //   "data_section_start" = .eh_frame_hdr start, NOT the GOT
```

So the parameter named `data_section_start` in `parse_eh_frame_pointer()` is really
"whatever the `datarel` base is *in this context*". Two call sites pass two different
things, and both are correct:

| Call site | `data_section_start` argument | Meaning |
|---|---|---|
| `eh_hdr::operator[]` (`dwarf.cpp:1593`) | start of `.eh_frame_hdr` | LSB spec rule for the search table |
| `parse_fde` (`dwarf.cpp:347`) | `0` | FDE `initial_location` never uses `datarel` in practice |
| `execute_cfi_instruction` (`dwarf.cpp:675`) | start of `.got.plt` | AMD64 psABI GOT base |

## 2.4 Where `execute_cfi_instruction` uses it

`execute_cfi_instruction` (`src/dwarf.cpp:666`) is now fully implemented, and the
consumer of these two locals is the `DW_CFA_set_loc` opcode.

Most CFI location advances are relative and tiny: `DW_CFA_advance_loc` (delta packed
into the opcode's low 6 bits), `DW_CFA_advance_loc1/2/4` (1/2/4-byte deltas). You can
see them in the real `hello_gsdb` unwind tables:

```
00000018 ... FDE cie=00000000 pc=0000000000001050..0000000000001076
  DW_CFA_advance_loc: 4 to 0000000000001054
  DW_CFA_undefined: r16 (rip)
```

`DW_CFA_set_loc` is the exception: instead of a delta it carries an **absolute
address, encoded with the FDE's pointer encoding** (the one from the CIE's `'R'`
augmentation, parsed at `dwarf.cpp:302-303` into `cie.fde_pointer_encoding`). The
implementation (`src/dwarf.cpp:707-715`):

```cpp
case DW_CFA_set_loc: {
    auto current_offset =
        elf.data_pointer_as_file_offset(cur.position());
    auto loc = parse_eh_frame_pointer(
        elf, cur, cie.fde_pointer_encoding, current_offset.off(),
        text_section_start.addr(), plt_start.addr(),
        fde.initial_location.addr());
    ctx.location = gsdb::file_addr{elf, loc};
    break;
}
```

That's it. `text_section_start` and `plt_start` are hoisted to the top of the function
because they're needed by that one case and are loop-invariant across the instruction
stream.

---

# Part 3 — Gotchas and correctness notes for this codebase

## 3.1 `.got.plt` is a proxy for the GOT base, and it's the wrong proxy under full RELRO

The psABI says the `datarel` base is `_GLOBAL_OFFSET_TABLE_`. Our code approximates
that with "start of `.got.plt`". Empirically:

| Binary | Link mode | `_GLOBAL_OFFSET_TABLE_` | `.got` start | `.got.plt` start |
|---|---|---|---|---|
| `pltdemo` | `-z lazy -z norelro` | `0x403338` | `0x403328` | `0x403338` ✓ match |
| `hello_gsdb` | full RELRO (`BIND_NOW`) | `0x3fb8` | `0x3fb8` | *absent* ✗ |

So on this project's own test targets the approximation yields `file_addr{}` (zero)
where the true GOT base is `0x3fb8`. It doesn't currently bite because GCC on x86-64
emits `pcrel` (`0x1b`) for FDE pointers and reserves `datarel` for the
`.eh_frame_hdr` table — which, per §2.3, uses a *different* base anyway. A
`datarel`-encoded `DW_CFA_set_loc` is exotic. Note that `execute_cfi_instruction`
*does* now consume `plt_start` (`src/dwarf.cpp:712`), so the only thing standing
between this approximation and a wrong answer is the encoding choice.

If you ever want it airtight, the robust lookup is: prefer the
`_GLOBAL_OFFSET_TABLE_` symbol (the `elf` class already has
`get_symbols_by_name()`), then fall back to `.got.plt`, then `.got`:

```cpp
// pseudo-sketch, not applied
auto got_base = [&] {
    auto syms = elf.get_symbols_by_name("_GLOBAL_OFFSET_TABLE_");
    if (!syms.empty()) return gsdb::file_addr{elf, syms[0]->st_value};
    if (auto s = elf.get_section_start_address(".got.plt")) return *s;
    return elf.get_section_start_address(".got").value_or(gsdb::file_addr{});
}();
```

Leaving it as-is is a defensible call — it matches the reference implementation this
project follows, and the branch is dead for GCC/Clang output on x86-64. Worth a
comment saying so, at most.

## 3.2 `eh_frame_pointer_encoding_size()` masks `0x7`, not `0x0f`

At `src/dwarf.cpp:388`:

```cpp
switch (encoding & 0x7) {
    case DW_EH_PE_absptr: return 8;   // 0x00
    case DW_EH_PE_udata2: return 2;   // 0x02
    case DW_EH_PE_udata4: return 4;   // 0x03
    case DW_EH_PE_udata8: return 8;   // 0x04
    default: error::send("Invalid pointer encoding!");
}
```

Masking `0x7` instead of `0xf` folds the signed variants onto the unsigned ones, and
by luck the sizes agree:

| Encoding | Value | `& 0x7` | Maps to | Size | Correct? |
|---|---|---|---|---|---|
| `sdata2` | `0x0a` | `0x2` | `udata2` | 2 | ✓ |
| `sdata4` | `0x0b` | `0x3` | `udata4` | 4 | ✓ |
| `sdata8` | `0x0c` | `0x4` | `udata8` | 8 | ✓ |
| `uleb128` | `0x01` | `0x1` | — | — | throws (correct: variable-size, no fixed size) |
| `sleb128` | `0x09` | `0x1` | — | — | throws (same) |

So it works, but it works by coincidence of the constant values rather than by
intent. `& 0xf` with explicit `sdata*` cases would say the same thing on purpose.

## 3.3 Lifetime

`get_section_start_address()` returns a `file_addr` by value, so unlike
`get_section_contents()` / `get_section_name()` there's no mmap-aliasing hazard here.
`file_addr` does hold a `const elf*` though, so it must not outlive the `elf`.

## 3.4 Terminology cheat sheet

| Name | What it is |
|---|---|
| `.plt` | Executable stubs, one per lazily-bound function, 16 bytes each |
| `.got` | Writable slots for data symbols and eagerly-bound functions (`GLOB_DAT`) |
| `.got.plt` | Writable jump slots for lazily-bound functions (`JUMP_SLOT`) + 3 reserved |
| `.plt.got` | Stubs for functions whose slot lives in `.got` (eager binding) — *not* the same as `.got.plt` |
| `.plt.sec` | Newer split-PLT variant emitted with `-z ibt`/CET |
| `.rela.plt` | Relocations describing the `.got.plt` slots |
| `_GLOBAL_OFFSET_TABLE_` | Symbol naming the GOT base; the true `DW_EH_PE_datarel` base |
| GOT[0] | Address of `.dynamic` |
| GOT[1] | `struct link_map*` — written by the loader at startup |
| GOT[2] | `_dl_runtime_resolve` — written by the loader at startup |
| PLT0 | The shared resolver trampoline at the top of `.plt` |

---

# Reproducing everything above

```bash
# --- a binary WITH .got.plt (lazy binding, no RELRO) ---
cat > /tmp/pltdemo.c <<'EOF'
#include <stdio.h>
#include <string.h>
int main(void) { puts("hi"); printf("%zu\n", strlen("hi")); return 0; }
EOF
gcc -O0 -no-pie -Wl,-z,lazy,-z,norelro /tmp/pltdemo.c -o /tmp/pltdemo

readelf -SW /tmp/pltdemo | grep -E '\.got|\.plt|\.text|\.dynamic'
readelf -sW /tmp/pltdemo | grep GLOBAL_OFFSET_TABLE
readelf -rW /tmp/pltdemo | grep -A10 'rela.plt'
objdump -d -j .plt --no-show-raw-insn /tmp/pltdemo
objdump -s -j .got.plt /tmp/pltdemo

# --- this project's targets: full RELRO, no .got.plt ---
readelf -SW build/test/targets/hello_gsdb | grep -E '\.got|\.plt|\.text'
readelf -dW build/test/targets/hello_gsdb | grep -i flags
readelf -sW build/test/targets/hello_gsdb | grep GLOBAL_OFFSET_TABLE

# --- the eh_frame side ---
readelf --debug-dump=frames       build/test/targets/hello_gsdb   # raw CFI opcodes
readelf --debug-dump=frames-interp build/test/targets/hello_gsdb  # decoded unwind table
```

Watch the lazy resolution happen live — break on `main`, dump GOT[3] before and after
the first `puts` call:

```
gdb -q /tmp/pltdemo
(gdb) break main
(gdb) run
(gdb) x/gx 0x403350          # -> 0x401036, i.e. puts@plt+6, unresolved
(gdb) next                   # step over the puts() call
(gdb) x/gx 0x403350          # -> real address inside libc
```

---

## Related notes in this directory

- `x86-64-rip-rsp-rbp.md` — register/stack-frame background that CFI rules describe
- `dwarf-range-lists.md`, `line-table.md` — other DWARF sections parsed by `dwarf.cpp`
