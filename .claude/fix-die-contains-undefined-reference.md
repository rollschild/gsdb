# Fix: undefined reference to `gsdb::die::contains(unsigned long) const`

## Symptom

Link step of the `gsdb` CLI fails:

```
undefined reference to `gsdb::die::contains(unsigned long) const'
```

referenced from `as_range_list()`, `low_pc()`, `high_pc()` in `src/dwarf.cpp`
(lines 545, 550, 557, 564, 629, 655).

## Root cause

`bool die::contains(std::uint64_t) const;` is **declared** at
`include/libgsdb/dwarf.hpp:234` and called in several `die` methods, but it is
**never defined** in `src/dwarf.cpp`. Sibling methods (`operator[]`, `low_pc`,
`high_pc`, `contains_address`) all have definitions — only `contains` is
missing one. Compilation succeeds (declaration visible); linking fails
(symbol absent from every object file).

This is a missing-definition error, not a CMake/linker-flags problem.

## Proposed change (NOT applied — source edits are owner-only)

Add to `src/dwarf.cpp`, e.g. immediately before `operator[]` (currently line 391):

```cpp
bool gsdb::die::contains(std::uint64_t attribute) const {
    auto& specs = abbrev_->attr_specs;
    return std::find_if(specs.begin(), specs.end(), [=](const auto& spec) {
        return spec.attr == attribute;
    }) != specs.end();
}
```

Mirrors the existing `operator[]` scan over `abbrev_->attr_specs`.
`<algorithm>` is already included (src/dwarf.cpp:3), so no new header needed.

## Verify

```bash
cmake --build build
```
