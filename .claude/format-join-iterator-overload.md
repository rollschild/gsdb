# Proposal: Iterator-based `format_join` overload

## Location

`tools/gsdb.cpp`, the `format_join` overloads at lines 180 and 208.

## Change

Add an iterator-based overload and refactor the existing range overload to delegate to it.

### New iterator-based overload (applied at `tools/gsdb.cpp:180`, before the range overload)

The applied version also takes a `byte_fmt` format-spec parameter (so callers such
as the `memory read` handler can ask for `"{:02x}"`), which means it formats via
`std::vformat_to` rather than `std::format_to`:

```cpp
template <std::input_iterator It, std::sentinel_for<It> S>
std::string format_join(It first, S last, std::string_view separator,
                        std::string_view byte_fmt = "{:#04x}") {
    std::string res;
    std::string_view sep = "";
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
    return res;
}
```

### Simplified range overload (applied at `tools/gsdb.cpp:206-223`; the old body is kept commented out)

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
