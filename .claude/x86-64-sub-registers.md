# x86-64 Sub-Register Layout (High/Low 8-Bit Registers)

On x86-64, `rax` is a 64-bit register. It's subdivided into smaller overlapping pieces that all share the same physical storage:

```
63                              31              15       8 7        0
+-------------------------------+---------------+--------+----------+
|                               |      eax      |   ah   |    al    |
|              rax (64-bit)                     |   ax (16-bit)     |
|                               |  eax (32-bit)                     |
+-------------------------------+---------------+--------+----------+
```

- **al** - low 8 bits (bits 0-7)
- **ah** - high 8 bits of the low 16 (bits 8-15)
- **ax** - low 16 bits (bits 0-15), i.e. ah:al
- **eax** - low 32 bits (bits 0-31)
- **rax** - full 64 bits

They all alias the same underlying storage. Writing to `al` changes bits 0-7 of `rax`; writing to `ah` changes bits 8-15.

## In This Codebase

This is reflected in `include/libgsdb/detail/registers.inc`. The macros encode these relationships through the `super` parameter (the parent register whose `ptrace` offset is reused):

```cpp
DEFINE_GPR_64(rax, 0),          // full 64-bit, DWARF reg 0
DEFINE_GPR_32(eax, rax),        // low 32, same offset as rax
DEFINE_GPR_16(ax, rax),         // low 16, same offset as rax
DEFINE_GPR_8H(ah, rax),         // bits 8-15, offset + 1
DEFINE_GPR_8L(al, rax),         // bits 0-7, same offset as rax
```

The `8H` (high byte) macro adds `+1` to the parent's offset to reach byte index 1 (bits 8-15), while `8L` (low byte) uses the same offset (byte 0, bits 0-7). This works because x86 is little-endian - the lowest byte is at the lowest address.

## Only the Original Four Have High-Byte Registers

The high-byte registers (`ah`, `bh`, `ch`, `dh`) only exist for `rax`, `rbx`, `rcx`, `rdx` - a legacy from the 8086. The newer registers (`r8`-`r15`, `rsi`, `rdi`, `rbp`, `rsp`) only have low-byte variants (`r8b`, `sil`, `dil`, etc.), which were added in x86-64.
