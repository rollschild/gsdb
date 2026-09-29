# SysV Eightbyte Classification — Why Field Classes Are Merged

> Explains the merge step in `classify_class_field()` (`src/type.cpp:205-215`):
> why the System V x86-64 ABI classifies aggregates per *eightbyte* rather than
> per field, what `merge_parameter_classes` does, what the two elements of
> `get_parameter_classes()`'s result mean, and why gsdb has to get this exactly
> right for inferior function calls.

```cpp
auto field_classes = field_type.get_parameter_classes();
// always merge the field’s first parameter class with the current
// parameter class in the eightbyte to which the field belongs
classes[eightbyte_index] =
    merge_parameter_classes(classes[eightbyte_index], field_classes[0]);
if (eightbyte_index == 0) {
    // The field may span 2 eightbytes, however, so we also merge the
    // second parameter classes if this field belongs to the first
    // eightbyte
    classes[1] = merge_parameter_classes(classes[1], field_classes[1]);
}
```

---

## Why merge at all: registers carry chunks, not fields

The ABI does **not** pass a struct field by field. It passes the struct's raw
bytes in 8-byte pieces (**eightbytes**), and each piece travels in one register.
An aggregate of 16 bytes or less is at most two eightbytes. Each eightbyte gets
exactly one class, and that class picks its register file:

| class     | goes in                              |
|-----------|--------------------------------------|
| `integer` | `%rdi`, `%rsi`, `%rdx`, `%rcx`, …    |
| `sse`     | `%xmm0`, `%xmm1`, …                  |
| `memory`  | the stack                            |

Take `struct S { int a; float b; };`. In memory it's 8 bytes:

```
offset:  0   1   2   3   4   5   6   7
        [    int a    ][   float b    ]
         \_________ eightbyte 0 ______/
```

A caller passing `S` by value doesn't load `a` into one register and `b` into
another. It loads the whole 8-byte chunk in one instruction:

```asm
movq  (%rsp), %rdi        # all 8 bytes of S, a and b together
call  f
```

The callee stores the chunk back to recreate the struct, then reads the fields
from memory:

```asm
movq  %rdi, -8(%rbp)      # S is back in memory
movl  -8(%rbp), %eax      # a
movss -4(%rbp), %xmm0     # b
```

Since `a` and `b` share one register, they can't have separate register classes.
The ABI needs **one class per eightbyte**, meaning one answer to "GPR, XMM, or
stack?" for all the bytes in that chunk.

### Why not give each field its own register?

That would use a lot of registers. A struct of 8 `char`s would need 8 of them.
Chunking caps any struct at **two registers**, and the callee rebuilds it with
at most two stores. The trade-off is that mixed chunks need a rule to pick one
class, and that rule is the merge.

---

## Merge turns several field classes into one

The classifier visits fields one at a time. Each field says what it would want by
itself: `int` wants `integer`, `float` wants `sse`. When several fields fall into
the same eightbyte, their wishes have to be combined into one verdict.
`merge_parameter_classes` does this as a running fold, starting from `no_class`:

```
classes[0] = no_class
visit a (int, offset 0)   → merge(no_class, integer) = integer
visit b (float, offset 4) → merge(integer, sse)      = integer
```

So `S` goes in `%rdi`.

### The merge rules (psABI §3.2.3, step 4)

Applied in order:

1. Same class on both sides: keep it.
2. One side is `no_class`: take the other.
3. Either side is `memory`: `memory`.
4. Either side is `integer`: `integer`.
5. Either side is `x87`, `x87up` or `complex_x87`: `memory`.
6. Otherwise: `sse`.

These follow from which register can actually hold the mixed bytes:

- **Everything is float data → `sse`.** Example: `{ float x, y; }`. The callee
  can use the values directly in `%xmm0`, which is the fast path for float-heavy
  structs like 2D points.
- **Any integer data → `integer`.** A GPR carries 8 opaque bytes and the callee
  spills them to memory anyway, so float bits riding along are fine. The ABI
  picks the GPR over the XMM register as the general-purpose carrier.
- **x87 mixed with anything, or anything `memory` → `memory`.** An 80-bit
  `long double` has no register it can share, so the whole struct goes on the
  stack.

### Examples

| struct                  | eightbyte 0                  | eightbyte 1 | passed in          |
|-------------------------|------------------------------|-------------|--------------------|
| `{ float a; float b; }` | sse ⊕ sse = **sse**          | –           | `%xmm0`            |
| `{ int a; float b; }`   | integer ⊕ sse = **integer**  | –           | `%rdi`             |
| `{ double d; long l; }` | **sse**                      | **integer** | `%xmm0`, `%rdi`    |

In the second row, the float ends up in a GPR along with the int. Rule 4 exists
for this case: once an eightbyte holds any integer data, the whole eightbyte
goes in a general-purpose register.

---

## What `field_classes[0]` and `field_classes[1]` mean

`field_type.get_parameter_classes()` (`src/type.cpp:504`) returns the same
`std::array<parameter_class, 2>` shape as `classes`, but for the **field's own
type**, counted from the start of the field:

- `[0]` is the class of the field's first eightbyte.
- `[1]` is the class of the field's second eightbyte. It is `no_class` if the
  field fits in 8 bytes.

Concretely:

| field type           | `get_parameter_classes()` |
|----------------------|---------------------------|
| `int` / `char*`      | `[integer, no_class]`     |
| `double`             | `[sse, no_class]`         |
| `long double`        | `[x87, x87up]`            |
| `int[3]` (12 bytes)  | `[integer, integer]`      |

Nested structs never reach this `else` branch. `classify_class_field()` recurses
into them member by member instead, passing the accumulated bit offset down.

---

## Why `[1]` is only merged when `eightbyte_index == 0`

- **Field starts in eightbyte 0:** it can spill into eightbyte 1 if it's longer
  than 8 bytes (`long double`, an array). Its `[0]` goes into `classes[0]` and
  its `[1]` into `classes[1]`. For a small field, `[1]` is `no_class`, and
  merging with `no_class` changes nothing (rule 2), so there's no need to check
  the field's size.
- **Field starts in eightbyte 1:** it has nowhere left to spill. It would go
  past 16 bytes, and those types are already sent to `memory` by the
  `byte_size() > 16` check in `classify_class_type()`. So only its `[0]`
  matters, and it goes into `classes[1]`.

The post-merger cleanup pass at the end of `classify_class_type()` then applies
the rules that cover the whole struct: if either eightbyte is `memory`, or
`x87up` isn't preceded by `x87`, the entire struct goes to memory.

---

## Why gsdb has to get this exactly right

When gsdb calls a function in the inferior with a struct argument, it has to put
the bytes in the same registers the compiled callee will read. If gsdb
classifies `S` as `sse` and loads it into `%xmm0`, the callee still reads `%rdi`
and gets garbage. The merge makes gsdb's classification match the compiler's.

See also `ntfpoc-and-unique-ptr-abi.md`: NTFPOC types skip this classification
entirely and are passed by address.
