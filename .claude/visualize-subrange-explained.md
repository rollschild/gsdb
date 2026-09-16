# `visualize_subrange` explained

Walkthrough of the array visualizer in `src/type.cpp` (`visualize_subrange`,
lines 69–95, and its caller `visualize_array_type`, lines 97–108, as of
2026-09-14).

The function turns the raw bytes of a C array, of any number of dimensions,
into a nested bracketed string such as `[[1, 2, 3], [4, 5, 6]]`. It does so
by peeling one dimension per recursion level and slicing the byte span
accordingly.

```cpp
std::string visualize_subrange(const gsdb::process& proc,
                               const gsdb::type& value_type,
                               gsdb::span<const std::byte> data,
                               std::vector<std::size_t> dimensions) {
    if (dimensions.empty()) {
        std::vector<std::byte> data_vec{data.begin(), data.end()};
        return gsdb::typed_data{std::move(data_vec), value_type}.visualize(
            proc);
    }

    std::string ret = "[";
    auto size = dimensions.back();
    dimensions.pop_back();
    auto sub_size =
        std::accumulate(dimensions.begin(), dimensions.end(),
                        value_type.byte_size(), std::multiplies<>());
    for (std::size_t i = 0; i < size; ++i) {
        gsdb::span<const std::byte> subdata{data.begin() + i * sub_size,
                                            data.end()};
        ret += visualize_subrange(proc, value_type, subdata, dimensions);

        if (i != size - 1) {
            ret += ", ";
        }
    }
    return ret + "]";
}
```

## 1. Inputs and how the caller prepares them

Take `int a[2][3]` as the running example. GCC emits one
`DW_TAG_subrange_type` child per dimension, outermost first:

```
DW_TAG_array_type
├── DW_AT_type ─────────────► DW_TAG_base_type "int"   byte_size = 4
├── DW_TAG_subrange_type      DW_AT_upper_bound = 1    →  2 elements  (outer)
└── DW_TAG_subrange_type      DW_AT_upper_bound = 2    →  3 elements  (inner)
```

The caller, `visualize_array_type`, collects `upper_bound + 1` for each
subrange and then reverses the vector, so the outermost dimension ends up at
the back where `pop_back()` can reach it cheaply:

```cpp
std::vector<std::size_t> dimensions;
for (auto& child : data.value_type().get_die().children()) {
    if (child.abbrev_entry()->tag == DW_TAG_subrange_type) {
        dimensions.push_back(child[DW_AT_upper_bound].as_int() + 1);
    }
}
std::reverse(dimensions.begin(), dimensions.end());
auto value_type = data.value_type().get_die()[DW_AT_type].as_type();
return visualize_subrange(proc, value_type, data.data(), dimensions);
```

```
collected:  [2, 3]      outer, inner
reversed:   [3, 2]      inner ... outer
                  ^
                back() = the dimension to peel first
```

The three arguments that flow into the recursion:

| argument     | meaning                                                        |
|--------------|----------------------------------------------------------------|
| `value_type` | the element type (`int`), from the array DIE's `DW_AT_type`    |
| `data`       | non-owning `gsdb::span` over the array's bytes                 |
| `dimensions` | per-dimension element counts, **innermost first**              |

`dimensions` is passed **by value**, so every recursion frame owns its own
copy. Popping in one frame never disturbs a sibling frame.

## 2. Memory layout and the stride computation

C arrays are row-major, so the whole of `a[0]` precedes the whole of `a[1]`:

```
byte offset   0     4     8     12    16    20    24
              ┌─────┬─────┬─────┬─────┬─────┬─────┐
              │a[0] │a[0] │a[0] │a[1] │a[1] │a[1] │
              │ [0] │ [1] │ [2] │ [0] │ [1] │ [2] │
              └─────┴─────┴─────┴─────┴─────┴─────┘
              └────── a[0] ──────┘└────── a[1] ──────┘
                 sub_size = 12       sub_size = 12
```

To step from `a[0]` to `a[1]` the code needs the byte size of one inner
sub-array. That is the element size multiplied by every dimension that is
still left after popping. The `std::accumulate` call is a left fold with
`value_type.byte_size()` as the seed and multiplication as the operator:

```
after pop_back():  dimensions = [3]

sub_size = byte_size(int) × 3
         =       4        × 3
         = 12
```

For a three-dimensional `int b[2][3][4]` the same rule holds at each level:

```
reversed dims = [4, 3, 2]

level 0   size = 2   remaining [4, 3]   sub_size = 4 × 4 × 3 = 48
level 1   size = 3   remaining [4]      sub_size = 4 × 4     = 16
level 2   size = 4   remaining []       sub_size = 4         =  4
level 3   base case (dimensions empty)  → visualize one int
```

Visually, for `b`:

```
                    ┌──────────────── 96 bytes ────────────────┐
level 0  (size 2)   │        b[0]         │        b[1]        │   stride 48
                    ├──────┬──────┬───────┼──────┬──────┬──────┤
level 1  (size 3)   │b[0][0]b[0][1]b[0][2]│b[1][0]b[1][1]b[1][2]│  stride 16
                    ├─┬─┬─┬┼─┬─┬─┬┼─┬─┬─┬─┼─┬─┬─┬┼─┬─┬─┬┼─┬─┬─┬┤
level 2  (size 4)   │ │ │ ││ │ │ ││ │ │ │ │ │ │ ││ │ │ ││ │ │ ││  stride 4
                    └─┴─┴─┴┴─┴─┴─┴┴─┴─┴─┴─┴─┴─┴─┴┴─┴─┴─┴┴─┴─┴─┴┘
```

## 3. The recursion, frame by frame

Each non-leaf frame opens a bracket, visits `size` sub-arrays, separates them
with `", "`, and closes the bracket. The `i != size - 1` guard prevents a
trailing comma. The leaf, reached when `dimensions` is empty, copies its
bytes into a vector and hands them to `typed_data::visualize` as a plain
`int`.

```
visualize_subrange(data[0..24), dims=[3,2])
│   size = 2, sub_size = 12                          returns "[" + ... + "]"
│
├── i=0  visualize_subrange(data[0..24), dims=[3])
│   │   size = 3, sub_size = 4
│   ├── i=0  visualize_subrange(data[0..24),  [])   leaf → "1"
│   ├── i=1  visualize_subrange(data[4..24),  [])   leaf → "2"
│   └── i=2  visualize_subrange(data[8..24),  [])   leaf → "3"
│                                                    returns "[1, 2, 3]"
└── i=1  visualize_subrange(data[12..24), dims=[3])
    ├── i=0  visualize_subrange(data[12..24), [])   leaf → "4"
    ├── i=1  visualize_subrange(data[16..24), [])   leaf → "5"
    └── i=2  visualize_subrange(data[20..24), [])   leaf → "6"
                                                     returns "[4, 5, 6]"

final:  "[[1, 2, 3], [4, 5, 6]]"
```

The string grows in the order the tree is walked:

```
"["  →  "[["  →  "[[1"  →  "[[1, 2"  →  "[[1, 2, 3"  →  "[[1, 2, 3]"
     →  "[[1, 2, 3], "  →  "[[1, 2, 3], [4, 5, 6]"  →  "[[1, 2, 3], [4, 5, 6]]"
```

### Control flow of one frame

```
        ┌──────────────────────────┐
        │ dimensions.empty() ?     │
        └──────┬───────────┬───────┘
           yes │           │ no
               ▼           ▼
   ┌────────────────┐   ┌────────────────────────────────┐
   │ copy data into │   │ ret = "["                      │
   │ vector, build  │   │ size = dims.back(); pop_back() │
   │ typed_data,    │   │ sub_size = elem × Π(remaining) │
   │ return its     │   └──────────────┬─────────────────┘
   │ .visualize()   │                  ▼
   └────────────────┘   ┌────────────────────────────────┐
                        │ for i in [0, size):            │
                        │   subdata = data[i×sub_size..) │
                        │   ret += recurse(subdata, dims)│
                        │   if not last: ret += ", "     │
                        └──────────────┬─────────────────┘
                                       ▼
                        ┌────────────────────────────────┐
                        │ return ret + "]"               │
                        └────────────────────────────────┘
```

## 4. One detail worth noticing: the spans are suffixes

Line 86 builds each child span as `{begin + i × sub_size, end}`, where `end`
is the **parent's** end, not `begin + (i+1) × sub_size`. So each child sees
everything from its start to the end of the whole array:

```
i=0   [############################]   bytes  0..24
i=1               [################]   bytes 12..24
      ^           ^
      only the first sub_size bytes of each window are "its" data
```

This is correct because every consumer reads only the leading `byte_size()`
bytes. The leaf's `typed_data` reads the first 4 bytes for an `int` and
ignores the rest. It does mean the leaf copies the entire remaining suffix
into a vector, so the total bytes copied grows quadratically with element
count:

```
leaf copies for int a[6] (24 bytes):

elem 0   ████████████████████████   24 bytes
elem 1       ████████████████████   20
elem 2           ████████████████   16
elem 3               ████████████   12
elem 4                   ████████    8
elem 5                       ████    4
                                    ── 84 bytes copied vs 24 needed
```

For debugger-sized arrays this is harmless.

**Fix, if you want it tight:** end the child span at
`data.begin() + (i + 1) * sub_size`, and the leaf copy becomes exactly one
element:

```cpp
gsdb::span<const std::byte> subdata{data.begin() + i * sub_size,
                                    data.begin() + (i + 1) * sub_size};
```

## 5. Related DWARF notes

- `gsdb::span` (in `include/libgsdb/types.hpp`) has a two-pointer constructor
  `span(T* data, T* end)`, which is what line 86 uses; it computes
  `size_ = end - data`.
- `visualize_array_type` assumes every subrange carries `DW_AT_upper_bound`.
  GCC omits it for flexible / incomplete arrays (`int a[]`), and some
  producers use `DW_AT_count` instead. Neither case is handled yet.
- The reverse-then-`pop_back` pattern is there because DWARF lists dimensions
  outermost first, while the recursion must consume the outermost dimension
  first and `pop_back` is the O(1) removal on a `std::vector`.
