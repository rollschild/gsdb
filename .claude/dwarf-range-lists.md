# DWARF Range Lists (`.debug_ranges`) — Noncontiguous Address Ranges

> A plain-language walkthrough of how DWARF describes a DIE (function, scope,
> etc.) whose code is **split across several non-adjacent regions of memory**,
> and why we need a dedicated `gsdb::range_list` type to handle it.

---

## The Problem

Normally a DIE says "I cover addresses from X to Y" with two attributes:

```
DW_AT_low_pc  = 0x1000   ← start
DW_AT_high_pc = 0x1050   ← end
```

That works when the code is **one contiguous block**:

```
        low_pc                      high_pc
          │                            │
          ▼                            ▼
  ────────█████████████████████████████────────►  memory addresses
          0x1000                     0x1050
          └────── one function ──────┘
```

But compilers often **split** a function into pieces scattered across the
binary (hot/cold splitting, optimization, inlining). Now a single high/low
pair can't describe it:

```
   function "foo" actually lives in 3 separate chunks:

  ──███████──────────████──────────────█████████────►
    0x1000 0x1040    0x2000 0x2010     0x3000  0x3030
    └─chunk1─┘       └chunk2┘          └──chunk3──┘
```

To describe this, DWARF uses a **range list** stored in the `.debug_ranges`
section, and the DIE just points to it.

---

## The `.debug_ranges` Encoding

The section is a stream of **entries**. Every entry is exactly **two
integers**, each the machine's address size (8 bytes on x64). So every entry
is 16 bytes:

```
┌────────────────┬────────────────┐
│   integer 1    │   integer 2    │   = one entry (16 bytes on x64)
│   (8 bytes)    │   (8 bytes)    │
└────────────────┴────────────────┘
```

There are **three kinds** of entry, distinguished by their values:

### 1. Regular range entry

The two integers are **offsets** from the current *base address*:

```
┌────────────────┬────────────────┐
│  begin offset  │   end offset   │   →  range = [base+begin, base+end)
└────────────────┴────────────────┘
```

### 2. Base address selector

First integer = **all bits set** (`0xFFFFFFFFFFFFFFFF`) → "I'm a selector".
Second integer = the **new base address**:

```
┌────────────────┬────────────────┐
│ 0xFFFF...FFFF  │   new base     │   →  change base for following entries
└────────────────┴────────────────┘
```

### 3. End-of-list

Both integers are **zero**:

```
┌────────────────┬────────────────┐
│       0        │       0        │   →  stop reading this list
└────────────────┴────────────────┘
```

---

## How the Base Address Works

The `begin`/`end` numbers in regular entries are **relative offsets**, not
absolute addresses. You add them to the current **base address** to get the
real address.

Where does the first base come from?

- **If a base selector appears first**, it sets the base.
- **If not**, the base is the `DW_AT_low_pc` of the referencing DIE.

> That's the subtle line at the end of the paragraph: such a DIE has a
> `DW_AT_low_pc` but **no** `DW_AT_high_pc`. The lone `low_pc` isn't describing
> a range — it's just supplying the **base address** for the range list.

---

## A Worked Example

Say `.debug_ranges` contains this for our 3-chunk `foo`, and the DIE's
`DW_AT_low_pc = 0x1000` (used as the initial base):

```
offset  entry                          interpretation
──────────────────────────────────────────────────────────────
 +0x00  [ 0x0000 , 0x0040 ]            base=0x1000 → [0x1000, 0x1040)
 +0x10  [ 0xFFFF...F , 0x3000 ]        BASE SELECTOR → base becomes 0x3000
 +0x20  [ 0x0000 , 0x0030 ]            base=0x3000 → [0x3000, 0x3030)
 +0x30  [ 0x0000 , 0x0000 ]            END OF LIST
```

Resulting set of covered ranges:

```
  base=0x1000                  base switched to 0x3000
       │                              │
  ──███████──────────────────────────█████████────────►
    0x1000  0x1040                  0x3000  0x3030
    [0x1000,0x1040)                 [0x3000,0x3030)
```

---

## The Reading Loop

```
   read 16 bytes ──► is int1 all-ones? ──yes──► set base = int2, continue
        ▲                  │no
        │                  ▼
        │          is int1==0 && int2==0? ──yes──► DONE
        │                  │no
        │                  ▼
        └──── emit range [base+int1, base+int2) ◄───────
```

---

## Why a New `range_list` Type

A simple `{low, high}` pair can hold one contiguous range. A range list can
hold **N ranges plus a shifting base**, so we introduce a `gsdb::range_list`
to wrap:

- decode the `.debug_ranges` bytes → a clean list of `[low, high)` ranges
- answer questions like *"does address A fall inside this DIE?"* across all
  the chunks.

**One key rule:** the ranges **can't overlap** — each address belongs to at
most one chunk, which keeps "does this DIE contain address A?" unambiguous.
