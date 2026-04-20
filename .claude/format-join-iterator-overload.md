# Proposal: Iterator-based `format_join` overload

## Location

`tools/gsdb.cpp`, after the existing `format_join` at line 127.

## Change

Add an iterator-based overload and refactor the existing range overload to delegate to it.

### New iterator-based overload (insert at line 127, before existing overload)

```cpp
template <std::input_iterator It, std::sentinel_for<It> S>
std::string format_join(It first, S last, std::string_view separator) {
    std::string res;
    std::string_view sep = "";
    for (; first != last; ++first) {
        if constexpr (std::same_as<std::iter_value_t<It>, std::byte>) {
            std::format_to(std::back_inserter(res), "{}{:#04x}", sep,
                           std::to_integer<std::uint8_t>(*first));
        } else {
            std::format_to(std::back_inserter(res), "{}{}", sep, *first);
        }
        sep = separator;
    }
    return res;
}
```

### Simplified range overload (replaces existing lines 127-143)

```cpp
template <std::ranges::range T>
// requires std::formattable<std::ranges::range_value_t<T>, char>
std::string format_join(const T& t, std::string_view separator) {
    return format_join(std::ranges::begin(t), std::ranges::end(t), separator);
}
```

## Rationale

- Mirrors `fmt::join(start, end, separator)` API.
- Uses C++20 iterator concepts (`std::input_iterator`, `std::sentinel_for`).
- The range overload delegates to the iterator overload, eliminating duplication.
- `std::iter_value_t<It>` is the iterator equivalent of `std::ranges::range_value_t<T>`.
