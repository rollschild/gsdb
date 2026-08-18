# Fix: `std::byte` has no `std::formatter` specialization

## Problem

`std::format` / `std::format_to` cannot format `std::byte` directly — there is no
`std::formatter<std::byte>` in the standard library. This causes a compile error in
`format_join()` when it is called with a `std::array<std::byte, N>`.

The error surfaced in the `format_join()` loop (that original body is still in the
tree, commented out, at `tools/gsdb.cpp:210-222`):

```cpp
std::format_to(std::back_inserter(res), "{}{}", separator, elem);
//                                                         ^^^^
// elem is std::byte — no formatter exists
```

## Two changes needed

### 1. `format_join` — convert `std::byte` to `std::uint8_t` before formatting (`tools/gsdb.cpp:185-202`)

Applied. `format_join` has since been split into an iterator/sentinel overload
(`tools/gsdb.cpp:180`) that does the work and a range overload
(`tools/gsdb.cpp:208`) that forwards to it. The byte format spec is now a
parameter (`byte_fmt`, defaulting to `"{:#04x}"`), so formatting goes through
`std::vformat_to` rather than `std::format_to`; the `if constexpr` that detects
`std::byte` elements and calls `std::to_integer<std::uint8_t>()` is the same idea:

```cpp
for (; first != last; ++first) {
    res += sep;
    if constexpr (std::same_as<std::iter_value_t<It>, std::byte>) {
        auto val = std::to_integer<std::uint8_t>(*first);
        std::vformat_to(std::back_inserter(res), byte_fmt,
                        std::make_format_args(val));
    } else {
        const auto& val = *first;
        std::vformat_to(std::back_inserter(res), byte_fmt,
                        std::make_format_args(val));
    }
    sep = separator;
}
```

### 2. `tools/gsdb.cpp:337` — remove the now-redundant `{:#04x}` format spec

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

### Bonus: `separator` vs `sep` bug (original code)

The original code passed `separator` (the function parameter) instead of `sep` (the
local variable that starts as `""`) to `format_to`. This would prepend a separator
before the first element. The current code appends `sep` itself before each element
(`tools/gsdb.cpp:186`) and only then assigns `sep = separator`, so the first element
gets no prefix.
