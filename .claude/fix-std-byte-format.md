# Fix: `std::byte` has no `std::formatter` specialization

## Problem

`std::format` / `std::format_to` cannot format `std::byte` directly — there is no
`std::formatter<std::byte>` in the standard library. This causes a compile error in
`format_join()` when it is called with a `std::array<std::byte, N>`.

The error surfaces at `tools/gsdb.cpp:116` (original code):

```cpp
std::format_to(std::back_inserter(res), "{}{}", separator, elem);
//                                                         ^^^^
// elem is std::byte — no formatter exists
```

## Two changes needed

### 1. `format_join` — convert `std::byte` to `std::uint8_t` before formatting (lines 118-126)

Already applied in working tree. Uses `if constexpr` to detect `std::byte` elements
and calls `std::to_integer<std::uint8_t>(elem)`:

```cpp
for (const auto& elem : t) {
    if constexpr (std::same_as<std::ranges::range_value_t<T>, std::byte>) {
        std::format_to(std::back_inserter(res), "{}{:#04x}", sep,
                       std::to_integer<std::uint8_t>(elem));
    } else {
        std::format_to(std::back_inserter(res), "{}{}", sep, elem);
    }
    sep = separator;
}
```

### 2. Line 143 — remove the now-redundant `{:#04x}` format spec

Since `format_join` already formats each byte as `0xff`, the returned `std::string`
just needs to be wrapped in brackets. `{:#04x}` is an integer format specifier and
cannot be applied to a `std::string` (compile error).

**Before:**
```cpp
return std::format("[{:#04x}]", format_join(t, ","));
```

**After:**
```cpp
return std::format("[{}]", format_join(t, ","));
```

### Bonus: `separator` vs `sep` bug (line 116, original code)

The original code passed `separator` (the function parameter) instead of `sep` (the
local variable that starts as `""`) to `format_to`. This would prepend a separator
before the first element. The working-tree fix correctly uses `sep`.
