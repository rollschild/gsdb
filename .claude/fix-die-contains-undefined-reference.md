# Fix: undefined reference to `gsdb::die::contains(unsigned long) const`

## Symptom

Link step of the `gsdb` CLI fails:

```
undefined reference to `gsdb::die::contains(unsigned long) const'
```

referenced from `as_range_list()`, `low_pc()`, `high_pc()` in `src/dwarf.cpp`
(now `src/dwarf.cpp:1115`, `1120`, `1127`, `1134`, `1199`).

## Root cause

`bool die::contains(std::uint64_t) const;` is **declared** at
`include/libgsdb/dwarf.hpp:448` and called in several `die` methods, but it was
**never defined** in `src/dwarf.cpp`. Sibling methods (`operator[]`, `low_pc`,
`high_pc`, `contains_address`) all have definitions — only `contains` was
missing one. Compilation succeeds (declaration visible); linking fails
(symbol absent from every object file).

This is a missing-definition error, not a CMake/linker-flags problem.

## Resolution (applied)

The definition now exists in `src/dwarf.cpp:954`, immediately before
`operator[]` (`src/dwarf.cpp:961`):

```cpp
bool gsdb::die::contains(std::uint64_t attribute) const {
    auto& specs = abbrev_->attr_specs;
    return std::find_if(std::begin(specs), std::end(specs), [=](auto spec) {
               return spec.attr == attribute;
           }) != std::end(specs);
}
```

Mirrors the existing `operator[]` scan over `abbrev_->attr_specs`.
`<algorithm>` is already included (src/dwarf.cpp:3), so no new header needed.

## Verify

```bash
cmake --build build
```
