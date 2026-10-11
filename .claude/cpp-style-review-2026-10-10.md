# C++ Style & Best-Practices Review — gsdb (re-review)

Date: 2026-10-10   Reviewer: Claude (`cpp-style-review` skill)   Scope: style/best practices only (no bug or security hunt)

This is the second review. The first one, `.claude/cpp-style-review-2026-09-06.md`, was written against `4915d55`; its line numbers were re-synced to the current tree today, so every location there is valid against HEAD (`74d1d4b`). This report does two things:

1. It re-checks all 63 findings from September (§5.0).
2. It reviews the roughly 2,700 lines added since `4915d55`: the type layer (`type.hpp`, `type.cpp`), variables, inferior calls and expression evaluation, CFI expression rules, thread-aware stepping, and the bug-audit fixes.

New findings continue the numbering at **#64**, so every ID is unique across both reports.

## 1. Executive summary

- **New findings: 10.** None is a must, 8 are should, and 2 are consider. The new code follows the same conventions as the rest of the library. The new problems are mostly copy-paste, parameter passing, and the way "which thread" is chosen.
- **Earlier findings:** of 63, 1 is fixed (#2), 1 is partly fixed (#11), and 61 are still open. 23 of them have new sites in the new code; §5.0 lists them.
- **Top items, in order of attention:**
  1. **#7 (must, open, now 3 sites).** Stored hit-handler lambdas capture `[&]`. Two new ones are at `src/target.cpp:1117` and `:1147`; they capture nothing, so `[]` is enough.
  2. **#64 (should).** `std::optional` values are dereferenced without a check in 5 places. In one of them, `variable read foo(` reads through an empty optional at `tools/gsdb.cpp:957`.
  3. **#67 (should).** The CFI expression parser is copy-pasted three times (`src/dwarf.cpp:767-834`). The copies swapped `expr_rule`/`val_expr_rule` (bug-audit M1), and none of them moves the cursor past the expression. That second problem is an `[sdb]` latent bug that is not in the bug audit. The same finding covers the duplicated `DW_AT_defaulted` test behind bug-audit H11a.
  4. **#65 (should).** 22 functions take a defaulted `std::optional<pid_t> otid = std::nullopt`. Calls that leave it out silently use the current thread. Bug-audit M7 and M11, and the `get_pc()` call in `step_out`, all come from this.
  5. **#69 (should).** `target::get_expression_result()` is `const` but rewrites the stored result through a `mutable` vector.
  6. **#68 (should).** `gsdb::die` holds a `std::vector`, yet it is passed by value to 3 functions and copied in 9 loops and locals. New read-only `std::string` parameters are passed by value, and sink parameters are copied instead of moved.
  7. **#70 (should).** Inner-scope names shadow outer ones within a few lines of each other: `data` in `visualize_class_type`, and the nested loop index `i` in `setup_arguments`.
  8. **#66 (should).** The CLI has its own copy of "read a variable as `typed_data`", which `target` already does internally.
  9. **#71 (should).** The new bit-field and ABI helpers narrow `std::uint64_t` to `std::uint8_t`, compare `int` template tags with `std::uint64_t`, and mask with negative `int`s.
  10. **Still open from September:** #1 (`elf` RAII, must) and #3 (copyable `pipe`, must).
- **Caveats:** none. The tree builds cleanly, every translation unit compiled under clang-tidy, and the guidelines snapshot is fresh.

## 2. Project profile

- **Standard and flags:** C++23 (`CMAKE_CXX_STANDARD 23`, required), built with GCC from the Nix shell. Flags are `-Wall -Wfatal-errors -Wextra -Werror -g -O1` (`CMakeLists.txt:32`), so warnings are errors. No sanitizers are configured.
- **Formatting and lint:** `.clang-format` is `BasedOnStyle: Google` with `IndentWidth: 4`. There is no `.clang-tidy`.
- **House rules that override generic guidance** (from `CLAUDE.md`):
  - Assembly is always AT&T syntax.
  - Factory types have private constructors with `friend` access, which is why `std::unique_ptr<T>(new T(...))` appears (#27).
  - X-macro tables are an accepted exception to "no macros"; only their hygiene is reviewed (#39).
  - Sources are report-only for this review.
  - `CLAUDE.md` names a refactor goal: move debugger primitives out of `tools/gsdb.cpp` into `libgsdb`. #66 builds on it.
- **Inventory:** 21 public headers plus `src/include/syscalls.inc`, 14 library sources, the CLI, the tests and 18 test-target sources. 13,054 lines in total, of which about 11.8k are reviewed C++ (the `detail/dwarf.h` and `syscalls.inc` constant tables are reviewed as data). New since the last review: `include/libgsdb/type.hpp` and `src/type.cpp` (950 lines), plus +681 lines in `src/target.cpp`, +346 in `src/dwarf.cpp`, +157 in `src/process.cpp` and +138 in `tools/gsdb.cpp`. Nothing is excluded except `build/` and the generated `compile_commands.json`.
- **Build health:** `cmake --build build` builds every target cleanly, and all 34 translation units compiled under clang-tidy. In September the in-progress `dwarf_expression::eval` code did not compile; that is resolved.

## 3. Knowledge base used

- Effective Modern C++ (42 items), through the skill's curated checklist.
- C++ Core Guidelines snapshot dated Jun 14, 2026, upstream commit `33bcd01` (2026-08-06), fetched 2026-09-06. Refresh status: fresh (34 days old against a 60-day threshold), so no refresh was needed.
- Other sources cited: Abseil Tips of the Week, the Google C++ Style Guide, LLVM Coding Standards, SEI CERT C/C++, *C++ Coding Standards*, *Effective C++*, *Effective STL*, cppbestpractices, and reference documentation (cppreference, CMake, POSIX).
- Every source is listed with its title and a link (or, for books, edition and ISBN) in Appendix B. Citations were re-checked against the sources on 2026-10-10.

## 4. Mechanical pass

clang-tidy 21.1.8 ran with the skill's curated check set over all 34 translation units; none had compile errors. It produced 1006 unique warnings, deduplicated by file, line and check, with headers deduplicated across translation units. Every warning below is a lead; a finding enters the report only after the code was read.

| check | hits | disposition |
|---|---|---|
| readability-qualified-auto | 138 | style choice; not reported |
| modernize-use-nodiscard | 115 | → #15 (consider) |
| cppcoreguidelines-pro-bounds-pointer-arithmetic | 87 | concentrated in `cursor` and the byte helpers; not reported |
| misc-const-correctness | 82 | → #21 (new sites in §5.0) |
| cppcoreguidelines-narrowing-conversions | 43 | → #30, #71 |
| readability-implicit-bool-conversion | 36 | → #37; `type.cpp:718, 724` is the H11a expression (#67) |
| modernize-use-designated-initializers | 36 | not reported |
| cppcoreguidelines-pro-type-reinterpret-cast | 34 | → #32; new byte↔`uint8_t` casts → #71 |
| performance-enum-size / cppcoreguidelines-use-enum-class | 33 / 25 | `detail/dwarf.h` constants → #40 |
| misc-use-internal-linkage | 33 | test-target globals; not reported |
| cppcoreguidelines-pro-type-vararg | 29 | `ptrace(...)` calls; not reported |
| misc-no-recursion | 23 | DIE-tree and type walks are recursive by nature; not reported |
| readability-math-missing-parentheses | 19 | → #31, #71 |
| performance-unnecessary-value-param | 19 | → #9, #68 |
| cppcoreguidelines-init-variables / pro-type-member-init | 18 / 16 | → #16, #28 |
| readability-else-after-return | 15 | → #37 |
| cppcoreguidelines-avoid-non-const-global-variables | 15 | test targets, plus the SIGINT global (#56) |
| cppcoreguidelines-special-member-functions | 13 | → #3 (other hits are deleted-copy types) |
| performance-unnecessary-copy-initialization / for-range-copy | 11 / 7 | → #68 |
| modernize-use-ranges | 11 | → #55 |
| cppcoreguidelines-macro-usage | 11 | X-macro helpers → #39 |
| google-explicit-constructor | 9 | → #11 (2 new sites in `type.hpp`) |
| everything else | ≤ 8 each | folded where relevant |

**clang-format:** 2 files would change, with 9 replacements, both in test targets (#73). In September there was no drift.

**Grep hotspots** (counts over `src/`, `include/`, `tools/` and `test/tests.cpp`; the September counts are in parentheses where tracked):

| hotspot | count |
|---|---|
| `reinterpret_cast` | 33 lines (24), 2 of them in comments |
| `const_cast<` | 6 (5) |
| `new T(...)` | 10, all private-constructor factories |
| `malloc`/`free(` | 5 |
| non-guard `#define` | 4 |
| `std::endl` | 2 |
| `NULL` | 1 |
| `catch (...)` | 3 (1) |
| `[&]`/`[=]` lambdas | 25 (20) |
| `typedef` | 0 |
| unscoped `enum` | 25, all in `detail/dwarf.h` |
| `goto` / `volatile` | 0 / 0 |
| `mutable` members | 7 (3) |
| `std::terminate`/`exit`/`abort` | 2 |
| `std::optional<pid_t> otid = std::nullopt` | 22 (new) |
| `[[maybe_unused]]` | 23 (19) |
| `write_by_id(…, true\|false)` | 9 (new) |
| `__builtin_*` | 2 |

---

## 5. Findings

### 5.0 Status of the 2026-09-06 findings

Full text for each of these is in `.claude/cpp-style-review-2026-09-06.md`; its line numbers are current. The "New sites" column lists only code added since then. "Fix" names the change for those new sites; the original finding's Fix still covers the rest.

| # | Sev | Title (short) | Based on (see Appendix B) | Status | New sites since `4915d55` → Fix |
|---|---|---|---|---|---|
| 1 | must | `elf` ctor without RAII members | CG E.6; CG C.31; CG R.1; CG C.49; CCS Item 13; EC++ Item 13 | open | — |
| 2 | must | `symbol_name_map_` keys dangle | CG SL.str.1; CG SL.str.2; CG R.12; CG R.10; CG Pro.lifetime; CERT MEM50-CPP | **fixed** (`fd6be79`) | — |
| 3 | must | `pipe` copyable | CG C.21; EMC++ Item 17; CCS Item 52; CCS Item 53; EC++ Item 6; CG ES.27 | open | — |
| 4 | should | Manual C resource pairs | CG R.1; CG R.10; CG R.12; CG E.19; CG CPL.3; CERT FIO51-CPP; CERT POS54-C; CG SL.io.2 | open | `test/tests.cpp` "Local variables" (`open("/dev/null")`/`close`). Fix: a test-local `unique_fd`. |
| 5 | should | `file_entry` into a growable `mutable` vector | CG R.3; CG ES.65; CG Pro.lifetime; EMC++ Item 16 | open | — |
| 6 | consider | `optional<T*>` returns | CG F.60; ToTW #163; CG C.32 | open | — |
| 7 | must | Stored callback captures `[&]` | CG F.53; CG F.54; EMC++ Item 31 | open, **+2** | `src/target.cpp:1117-1119`, `:1147-1149` (`install_hit_handler([&] { return false; })`). Fix: `[]`, since nothing is used. |
| 8 | should | `bool` params / bare literals | CG I.24; CG I.4; ToTW #94; CG Enum.2 | open, **+16** | New `bool` params: `attr::as_expression`, `as_location_list`, `as_evaluated_location(…, bool in_frame_info)` (`include/libgsdb/dwarf.hpp:257, 261, 266`); `dwarf::index_die(…, bool in_function = false)` (`:515`). Bare literals: `as_evaluated_location(…, false)` (`src/target.cpp:98-99`; `tools/gsdb.cpp:938-940, 971-972`); `write_by_id(…, true)` ×9 (`src/target.cpp:191, 297, 369, 388, 390, 1121`; `src/process.cpp:1034, 1042`; `src/dwarf.cpp:883` with `false`). Fix: `enum class frame_info { no, yes }` and `enum class commit { no, yes }`, or `/*in_frame_info=*/false` at call sites, the way `set_ptrace_options(…, /*kill_on_tracer_exit=*/true)` already does. |
| 9 | should | Parameter passing | CG F.16; CG F.18; EMC++ Item 41; ToTW #117; ToTW #77; CG P.9 | open, **+many** | → #68 |
| 10 | should | `unique_ptr<target>&` params | CG R.30; CG F.7; ToTW #188 | open | — |
| 11 | should | Non-`explicit` single-arg ctors | CG C.46; Google "Implicit Conversions" | **partly fixed** (`dwarf`, `501504a`), **+2** | `type(die)`, `type(builtin_type)` (`include/libgsdb/type.hpp:34-35`). `strip()` depends on the implicit `die → type` conversion through a round trip, `ret = ret.get_die()[DW_AT_type].as_type().get_die();` (`:59`). Fix: make `type(die)` `explicit` and write `ret = ret.get_die()[DW_AT_type].as_type();`. Keeping `type(builtin_type)` implicit for literal arguments is defensible, but say so in a comment. |
| 12 | should | Unused params, stale `[[maybe_unused]]`, dead locals | CG F.9; CERT MSC13-C; CERT MSC12-C; cppreference `[[maybe_unused]]` | open, **+4** | `[[maybe_unused]] auto bit_size` (`src/type.cpp:213`); `[[maybe_unused]] auto param_classes` (`src/target.cpp:304`), which runs the whole classification and throws the result away; `[[maybe_unused]] auto& dwarf` (`src/target.cpp:1035`); `[[maybe_unused]] const gsdb::process& proc` (`src/dwarf.cpp:2294`). Fix: delete the locals and leave the parameter unnamed. |
| 13 | should | `up()`/`down()` preconditions | CG I.5; CG I.6; CG ES.104 | open | — |
| 14 | should | Small API-shape issues | CG F.49; CG NL.25; CG I.13; CG F.24 | open | — |
| 15 | consider | `[[nodiscard]]` | CG E.25; cppreference: `[[nodiscard]]` attribute; clang-tidy: `modernize-use-nodiscard` | open | `find_variable`, `resolve_indirect_name`, `evaluate_expression`, `read_location_data`, `inferior_malloc`. |
| 16 | should | Read-before-write members | CG ES.20; CG C.41; CG C.48; CCS Item 19; EC++ Item 4 | open | — |
| 17 | should | Assign in ctor body | CG C.49; CCS Item 48; EC++ Item 4 | open | — |
| 18 | should | `breakpoint` protected data and duplication | CG C.133; CG ES.3; CG F.1; CCS Item 41 | open | — |
| 19 | should | Virtual `resolve()` from ctors | CG C.82; CG C.50; EC++ Item 9; CERT OOP50-CPP | open | — |
| 20 | should | `dynamic_cast` chain | CG C.146; CG C.153; CCS Item 90 | open | — |
| 21 | should | Const-correctness holes | CG Con.2; CG Con.3; CG Con.4; EC++ Item 3; EC++ Item 28; CG Con.5 | open, **+1** | `std::array<gsdb::register_id, 6> int_regs` and `sse_regs` are rebuilt on every call (`src/target.cpp:260-270`). Fix: `static constexpr std::array`. |
| 22 | should | `const_cast` const-overload idiom | CG ES.50; CG ES.3; EC++ Item 3; cppreference: Explicit object member functions | open | — |
| 23 | should | Comparison operators | CG C.86; CG C.161; cppreference: Default comparisons | open, **+1** | `type::operator!=` is hand-written next to `operator==` (`include/libgsdb/type.hpp:96-97`). C++20 rewrites `!=` from `==`. Fix: delete `operator!=`. |
| 24 | should | `eh_hdr::parent` unused | CG C.12; CERT MSC13-C | open | — |
| 25 | consider | Anonymous union | CG C.181; CG C.182; CG P.4 | open | — |
| 26 | consider | `process` responsibilities | CG F.2; CCS Item 5; CG C.9 | open | It gained `read_string` and `inferior_call` (`include/libgsdb/process.hpp:252-263`). |
| 27 | consider | `unique_ptr<T>(new T)` factories | CG C.150; EMC++ Item 21; ToTW #134 | open (note only) | — |
| 28 | should | Declare-then-assign | CG ES.20; CG ES.22; CG ES.6; CG ES.10; CG ES.28; CCS Item 18; CCS Item 19 | open, **+4** | `gsdb::register_id reg;` before a `switch` (`src/target.cpp:349`); `gsdb::virt_addr call_addr;` before an `if`/`else` (`:453`); `std::uint64_t cfa;` (`src/dwarf.cpp:869`); `std::vector<std::byte> fixed_data; fixed_data.resize(n);` (`:2303-2304`). Fix: an immediately-invoked lambda or a `?:` expression, and `std::vector<std::byte> fixed_data(storage_byte_size);`. |
| 29 | should | Magic constants | CG ES.45; CCS Item 17; CG Con.5 | open, **+8** | 8/16-byte sizes (`src/type.cpp:366-371, 378, 388-390`); `(size + 7) & ~7` and `rsp &= ~0xf` (`src/target.cpp:285, 377`); eightbyte stride `8` (`:348-366`); `4096` (`:879`); `1024` (`src/process.cpp:1011`); `rsp -= 8` (`:1038`). Fix: `inline constexpr std::size_t eightbyte = 8;`, `stack_alignment = 16`, `pointer_size = 8`, `linux_path_max = 4096`. |
| 30 | should | Signed/unsigned in address math | CG ES.100; CG ES.102; CG ES.106; CG ES.104; CG ES.46 | open | → #71 for the new code |
| 31 | should | Bit manipulation on signed `int` | CG ES.101; CG ES.46; CG ES.41 | open | → #71 for the new code |
| 32 | should | Avoidable `reinterpret_cast`s | CG ES.48; CG ES.49; CG P.4; CG C.90; CG SL.con.4; CCS Item 91; CCS Item 92; cppreference `std::ignore` | open, **+7** | `src/target.cpp:129, 940, 989, 991`; `src/dwarf.cpp:2306-2307`; `test/tests.cpp` (DWARF expression test). Two more sit in commented-out code (#34). Fix: see #71 (span-based `memcpy_bits`) and #67 (`to_byte_vec`). |
| 33 | should | Names that shadow type names | CG ES.12; CG NL.19; cppbestpractices (warnings) | open, **+6** | Locals named `die` or `type` that shadow the classes (`src/type.cpp:118-119, 424, 466, 615, 746`); `type(die die)` and `type(builtin_type type)` (`include/libgsdb/type.hpp:34-35`). Fix: `d`, `ty`, `type_die`, and so on. |
| 34 | should | Commented-out code | LLVM "Comment Formatting"; CERT MSC12-C; CG NL.3 | open, **+2** | `src/target.cpp:959-964`; `tools/gsdb.cpp:1020-1031`. Fix: delete. |
| 35 | should | Two spellings of logical operators | CCS Item 0; cppreference: Alternative operator representations; clang-tidy: `readability-operators-representation` | open | — |
| 36 | consider | Loop shapes | CG ES.71; CG ES.73; CG ES.75; CG ES.3; CG P.9 | open | — |
| 37 | consider | Readability nits | CG ES.78; cppreference: `std::unreachable`; CG ES.87; ToTW #141; CG ES.56; LLVM "Don't use else after a return" | open, **+8** | `case X: error::send(...);` followed by the next label, inside `visualize_base_type` (`src/type.cpp:135-136, 151-153, 168-170, 175-176`); `else` after `return` (`src/type.cpp:510`, `src/target.cpp:133, 149, 945`, `src/dwarf.cpp:2226`); `if (…) return true; return false;` (`src/type.cpp:579-583`). Fix: as in #37. |
| 38 | should | Enum arithmetic for debug registers | CG Enum.4; CG ES.3; cppreference: `std::to_underlying` | open | — |
| 39 | should | X-macro helper macros leak | CG ES.33; CG ES.31; CG ES.32 | open | — |
| 40 | consider | Enum and constant hygiene | CG Enum.3; EMC++ Item 10; CG Enum.6; CG P.4; CG ES.27; CG SL.con.1; CG Con.5; EMC++ Item 15 | open | — |
| 41 | should | Unconstrained templates | CG T.10; CG T.11; CG I.9; CG SL.con.4; CG C.90 | open, **+2** | `to_byte_span<From>` and `to_byte_vec<From>` (`include/libgsdb/bit.hpp:73-83`) `memcpy` any type. Fix: `requires std::is_trivially_copyable_v<From>`. |
| 42 | should | Function template specialization | CG T.144; CCS Item 66; Herb Sutter, "Why Not Specialize Function Templates?" | open | — |
| 43 | should | Hand-rolled `gsdb::span` | CG ES.1; CG T.47; CG P.4; CG I.13; cppreference: `std::span`; cppreference: `std::as_bytes` | open | — |
| 44 | should | `errno` read late | CG E.28; CERT ERR30-C | open | — |
| 45 | should | Library prints and terminates | CG E.2; CERT ERR50-CPP | open | — |
| 46 | should | `catch` by non-const ref, catch-all | CG E.15; CG E.17 | open | — |
| 47 | should | Unchecked external input | CG SL.io.2; CG I.5; CERT ERR62-CPP; CERT STR37-C; cppreference `std::isdigit`; CERT POS54-C | open, **+1** | `std::isdigit(arg[0])` on a plain `char` (`src/target.cpp:141`). Fix: `std::isdigit(static_cast<unsigned char>(arg[0]))`. |
| 48 | consider | Error-policy consistency | CG E.1; CG E.14; CG I.5; CG I.6; CERT ERR62-CPP; POSIX `fork()`; cppreference `EXIT_FAILURE`; CG C.36; CG E.17 | open, **+2** | New deliberate catch-alls: `target::notify_stop` (`src/target.cpp:536-541`) and `~process` (`src/process.cpp:174-175`). Both are justified (best-effort unwind; CG C.36). Fix: one comment on each saying why. |
| 49 | should | Mixed include styles | CG SF.12; CG SF.10; Google "Names and Order of Includes"; Google "Include What You Use" | open | `type.hpp` and `type.cpp` repeat the angle/quoted split. |
| 50 | should | Heavy includes | CG SF.9; EC++ Item 31; CCS Item 22; cppbestpractices (fwddecl) | open, **+1** | `target.hpp` now includes `type.hpp` (needed: `std::optional<typed_data>` member). See #72. |
| 51 | consider | Header naming, C headers | CG SF.1; CG NL.27; cppreference "C compatibility headers"; Google "The #define Guard" | open | — |
| 52 | should | `count()` then `at()` | CG P.9; CG ES.1; ESTL Item 45; cppreference: associative-container `contains` | open, **+1** | `!seen.count(name)` (`tools/gsdb.cpp:937`). Fix: `contains`. |
| 53 | should | Output mechanisms | CG SL.io.50; CG SL.io.3; CG SL.io.1; cppbestpractices (endl); cppreference: `std::println` | open, **+2** | `std::cerr << "Variable not found!";` with no newline, next to `std::print` (`tools/gsdb.cpp:967`); `std::print("None")` with no newline (`:984`). Fix: `std::println(stderr, …)` and `std::println`. |
| 54 | should | `__builtin_ctzll` | CG P.2; CG ES.1; cppreference: `std::countr_zero` | open | — |
| 55 | consider | Standard-library idiom nits | ESTL Item 4; cppreference `starts_with`; cppreference `std::views::split`; cppreference `std::ranges::to`; cppreference "C compatibility headers"; ToTW #144 | open, **+6** | `arg.find(".") != npos` (`src/target.cpp:142`) → `arg.contains('.')`; `std::reverse`/`std::find_if`/`std::count` with `begin()`/`end()` (`src/type.cpp:112, 468`; `src/target.cpp:333-337`) → `std::ranges::*`; `push_back(std::pair{&elf, sym})` (`src/target.cpp:732`) → `emplace_back(&elf, sym)`; `insert(std::make_pair(…))` (`src/dwarf.cpp:1437, 1441`) → `emplace`; `using namespace gsdb;` in a function that still qualifies half its names (`src/type.cpp:183-203`) → `using enum gsdb::parameter_class;`. |
| 56 | should | SIGINT handler through a global | CG I.2; CERT SIG30-C; CERT SIG31-C; CERT MSC54-CPP | open | — |
| 57 | consider | Document single-threaded caches | EMC++ Item 16; CG CP.1 | open, **+3** | `type::byte_size_` (`include/libgsdb/type.hpp:112`), `dwarf::global_variable_index_` and `member_function_index_` (`include/libgsdb/dwarf.hpp:534, 539`). `expression_results_` is not a cache; see #69. |
| 58 | should | `get_x()` vs `x()` accessors | CG NL.8; Google "Function Names"; CCS Item 0 | open | New `get_die`, `get_builtin_type`, `get_bitfield_information`, `get_member_function_definition` next to `value_type()`, `address()`, `data()`. |
| 59 | should | Tutorial and stale comments | CG NL.1; CG NL.2; CG NL.3 | open, **+6** | Tutorial: parameter pack and fold expression (`include/libgsdb/type.hpp:49-58`), "mutable because…" (`:110-111`), `~dest_mask` (`include/libgsdb/bit.hpp:64`). Stale: "into an `sdb::typed_data` object" (`src/target.cpp:115`); `// TODO` above code that already does the job (`:124`); "demangle it and return it" when it no longer demangles (`:777-779`). Fix: as in #59. |
| 60 | consider | Identifier spelling and layout | CG NL.8; CG NL.16; cppbestpractices (codespell) | open, **+5** | Local `die_` with the member suffix (`src/type.cpp:374`); comment typos `pionts` (`src/type.cpp:564`), `direcly` (`include/libgsdb/elf.hpp:105`), `currect` (`src/target.cpp:1095`), `share lib` (`:888`). |
| 61 | should | 2005-era warning set | CCS Item 1; EC++ Item 53; CG P.12; cppbestpractices (warnings) | open | `-Wshadow` would have caught all of #70. |
| 62 | should | No `.clang-tidy`; dead GTest | CG P.12; CCS Item 2; cppbestpractices (analysis) | open | — |
| 63 | consider | CMake modernization | CMake docs: `include_directories()`; CMake docs: `CMAKE_<LANG>_COMPILER`; CMake docs: `cmake-presets(7)`; Henry Schreiner et al., *An Introduction to Modern CMake* | open | — |

### 5.1 Ownership & lifetime

### 64. `std::optional` dereferenced without a check — should
Rules: CG ES.65 “Don't dereference an invalid pointer” (an empty `std::optional` is the same hazard as an invalid pointer); CG SL.4 “Use the standard library in a type-safe manner”; CG I.5 “State preconditions (if any)”.
Where: 5 sites.
- `tools/gsdb.cpp:957`: `data.variable->visualize(...)`.
- `src/target.cpp:163`: `return *res.variable;`.
- `:297`: `return_slot->addr()`.
- `:826`: `debug_info->r_brk`.
- `:1173`: `*res.address()`.
- `test/tests.cpp` adds 7 `.variable->` uses, which are acceptable in tests.

What:
```cpp
auto data = target.resolve_indirect_name(name, pc);
auto str = data.variable->visualize(target.get_process());   // gsdb.cpp:956-957
```
Why: `resolve_indirect_name` returns `variable == std::nullopt` whenever the name ends in a call (`foo(`, `obj.method(`). So `variable read foo(` reads through an empty optional, which is undefined behaviour rather than an error message. This one is a latent defect. The other four sites are safe only because of an invariant set up elsewhere, and nothing at the dereference says so:
- `return_slot` is engaged exactly when the callee has `DW_AT_type`.
- Expression results always carry an address.
- `r_debug` was just read from a non-zero address.

Fix:
- At the CLI, check first: `if (!data.variable) { std::println(stderr, "Not a variable"); return; }`.
- In the library, use `error::send` when the optional is empty. Avoid `.value()`, because `std::bad_optional_access` is not a `gsdb::error` and escapes the REPL's `catch` (bug-audit C8).
- Where an invariant holds, make the type carry it. For example, pass `virt_addr return_slot` to `setup_arguments` and `read_return_value` only on the path that has one.

### 5.2 Interfaces & functions

### 65. Thread selection hides behind 22 defaulted `otid` parameters — should
Rules: CG I.1 “Make interfaces explicit” (its “bad” example is a hidden call mode that makes two identical calls behave differently); Google C++ Style Guide, “Default Arguments” (it bans defaults whose value depends on when they are evaluated; here the default is constant, `std::nullopt`, but it *means* “whichever thread is current”, which is the same trap).
Where:
- **Declarations:** 22 of the form `std::optional<pid_t> otid = std::nullopt`, 10 in `include/libgsdb/process.hpp` (including `inferior_call`, `:258-263`) and 12 in `include/libgsdb/target.hpp`.
- **Library calls inside thread-aware code that leave it out**, and so use the current thread:
  - `src/stack.cpp:31`: `reset_inline_height` (bug-audit M11).
  - `src/target.cpp:716`: `step_out` compares `process_->get_pc()` while stepping `tid`.
  - `src/target.cpp:1096` and `:1121`: `inferior_malloc` has no thread parameter at all, although its caller `parse_argument` (`:117-127`) has one.
  - `src/process.cpp:791-846`: the hardware stoppoint helpers (bug-audit M7).
  - `src/disassembler.cpp:19`.

What:
```cpp
stop_reason step_out(std::optional<pid_t> otid = std::nullopt);
...
if (!reason.is_breakpoint() or process_->get_pc() != return_address)   // target.cpp:716
```
Why: the default makes "which thread?" invisible at every call site. Forgetting the argument compiles, and the result is wrong only in multi-threaded inferiors, which is exactly how M7 and M11 got in.
Fix:
- Inside `libgsdb`, take `pid_t tid` with no default.
- Resolve `current_thread()` once at the API boundary: in the CLI, or in a thin set of defaulted overloads on `target` only.
- Give `inferior_malloc` a `tid` parameter.
- Afterwards, `grep -n 'get_pc()\|get_registers()' src/` should find nothing outside those wrappers.

### 66. The CLI re-implements "read a variable as `typed_data`" — should
Rules: CG ES.3 “Don't repeat yourself, avoid redundant code”; CG F.1 “"Package" meaningful operations as carefully named functions”; CG A.2 “Express potentially reusable parts as a library”.
Where:
- `tools/gsdb.cpp:926-950` (`handle_variable_locals_command`) repeats `get_initial_variable_data` (`src/target.cpp:92-111`): evaluate `DW_AT_location` against the selected frame's registers, call `read_location_data`, wrap the bytes in a `typed_data`.
- `tools/gsdb.cpp:961-999` (`handle_variable_location_command`) walks the `dwarf_expression::result` alternatives that `target::read_location_data` also walks.
- The commented-out block at `:1020-1031` is a third copy.

What:
```cpp
auto loc = var[DW_AT_location].as_evaluated_location(target.get_process(), target.get_stack().current_frame().regs, false);
auto value = target.read_location_data(loc, type.byte_size());
auto str = gsdb::typed_data{std::move(value), type}.visualize(target.get_process());
```
Why: every copy has to track the same details, such as which frame's registers to use and whether to record the address. The CLI copy already differs: it drops the address, so a value from `variable locals` could not be passed by reference later.
Fix: add `typed_data target::read_variable(const die& var, pid_t tid) const`, built from the body of `get_initial_variable_data` after the lookup. Use it from `get_initial_variable_data` and from `handle_variable_locals_command`, and delete the commented block.

### 67. Copy-pasted blocks in the new DWARF, type and call code — should
Rules: CG ES.3 “Don't repeat yourself, avoid redundant code”; CG F.1 “"Package" meaningful operations as carefully named functions”.
Where:
- **CFI expression rules,** `src/dwarf.cpp:767-775` (`DW_CFA_def_cfa_expression`), `:815-824` (`DW_CFA_expression`) and `:825-834` (`DW_CFA_val_expression`). Each copy reads `len` and builds `dwarf_expression{elf.get_dwarf(), {cur.position(), cur.position() + len}, true}`.
  - The swap of `expr_rule` and `val_expr_rule` (bug-audit M1) happened inside this copy-paste.
  - None of the three copies advances the cursor past the expression (`cur += len`). The CFI loop therefore decodes the next "opcode" from inside the DWARF expression. sdb has the same three copies, so this is an inherited latent bug, and it is **not yet in the bug audit**.
- **`index_die`,** `src/dwarf.cpp:1433-1443`: the `DW_AT_specification` and `DW_AT_abstract_origin` branches differ only in the attribute name.
- **`is_non_trivial_for_calls`,** `src/type.cpp:716-728`: the copy/move-constructor branch and the destructor branch repeat `!child.contains(DW_AT_defaulted) or ((!child[DW_AT_defaulted].as_int()) != DW_DEFAULTED_in_class)`. That is the inverted test from bug-audit H11a, written twice; clang-tidy also flags its `bool → int` conversion.
- **"Use the entry point as the return address and silence its handler":** `src/target.cpp:1116-1119` and `:1146-1149`.
- **`read_location_data`'s `get_bytes` lambda** (`src/target.cpp:938-943`) re-implements `to_byte_vec` (`include/libgsdb/bit.hpp:77-82`) with a `reinterpret_cast`.
- **Minor:** in `get_parameter_classes`, 4 and 5 `case` labels carry identical bodies (`src/type.cpp:593-609, 619-640`).

Why: the duplicates have already drifted twice (M1 and H11a), and the cursor bug sits in all three CFI copies.
Fix:
- `dwarf_expression read_cfi_expression(cursor& cur, const gsdb::elf& elf)`, which also does `cur += len`, used by the three CFI cases.
- `void index_definition(const die& d, std::uint64_t attr)` for `index_die`.
- `bool is_user_provided(const gsdb::die& fn)` in `type.cpp`, which also fixes H11a once.
- `virt_addr target::prepare_call_return_address()` for the two entry-point blocks.
- `std::visit([](auto v) { return to_byte_vec(v); }, reg_value)` instead of `get_bytes`.
- Stacked `case` labels in `get_parameter_classes`.

### 68. Heavy values passed and iterated by value in the new code — should
This continues #9.
Rules: CG F.16 “For "in" parameters, pass cheaply-copied types by value and others by reference to `const`”; CG F.18 “For "will-move-from" parameters, pass by `X&&` and `std::move` the parameter”; CG P.9 “Don't waste time or space”; EMC++ Item 41 “Consider pass by value for copyable parameters that are cheap to move and always copied”; ToTW #77 “Temporaries, Moves, and Copies”; ToTW #117 “Copy Elision and Pass-by-value”; ToTW #144 “Heterogeneous Lookup in Associative Containers” (`std::string_view` lookups).
Where:
- **`gsdb::die` copied.** It owns a `std::vector<const std::byte*>`, and the `children()` iterator returns `const die&`, so `auto` copies.
  - By-value parameters: `setup_arguments(…, gsdb::die func, …)`, `read_return_value(…, gsdb::die func, …)` and `inferior_call_from_dwarf(…, gsdb::die func, …)` (`src/target.cpp:257, 393, 447`).
  - Loop copies: `for (auto child : ….children())` at `src/type.cpp:226, 275, 316, 752, 778` and `src/dwarf.cpp:1463`.
  - Local copies: `auto var = *it;` (`src/type.cpp:475`); `auto frame_to_skip = inline_stack[…]` and `auto current_frame = …` (`src/target.cpp:655, 699`).
- **Read-only `std::string` passed by value:** `dwarf::find_global_variable` and `find_local_variable` (`include/libgsdb/dwarf.hpp:483, 485`), `target::find_variable` (`include/libgsdb/target.hpp:163`), and `get_initial_variable_data(…, std::string name, …)` (`src/target.cpp:81-83`). Also `std::optional<gsdb::typed_data> object`, passed by value and only read (`:170`).
- **Sinks copied instead of moved:**
  - `typed_data(…, type value_type, …) : …, type_(value_type)` (`include/libgsdb/type.hpp:119-121`).
  - `gsdb::typed_data{member_data, subtype}` (`src/type.cpp:62-63`).
  - `return {fixed_data, type_};` (`src/dwarf.cpp:2310`).
  - `auto name = args[2];` (`tools/gsdb.cpp:954, 963`).

Why: every `die` copy is a heap allocation. These loops run for every member of every type during ABI classification and printing, and the classification itself is recursive.
Fix:
- Use `const gsdb::die&` parameters and `const auto&` loop variables and locals.
- Take `std::string_view` for lookups. `fd6be79` already added a transparent `string_hash` for `symbol_name_map_`; reuse it for `function_index_` and `global_variable_index_`.
- Move the sinks: `type_(std::move(value_type))`, `typed_data{std::move(member_data), subtype}`, `return {std::move(fixed_data), type_};`.
- Bind CLI arguments without copying: `const auto& name = args[2];`.

### 5.3 Classes

### 69. `get_expression_result() const` mutates observable state through `mutable` — should
Rules: CG Con.2 “By default, make member functions `const`” (a function should be `const` unless it changes the object's observable state); CG ES.50 “Don't cast away `const`” (its example says caching and memoization are what `mutable` is for); EMC++ Item 16 “Make const member functions thread safe” (`mutable` members written from `const` functions).
Where: `include/libgsdb/target.hpp:174` (`const typed_data& get_expression_result(std::size_t i) const;`) and `:208` (`mutable std::vector<typed_data> expression_results_;`); `src/target.cpp:1163-1176`.
What:
```cpp
auto& res = expression_results_[i];
auto new_data = process_->read_memory(*res.address(), res.value_type().byte_size());
res = typed_data{std::move(new_data), res.value_type(), res.address()};   // in a const function
return res;
```
Why: this is not a cache. It replaces the stored result with fresh inferior memory, and every later reader sees the new value. Because the function is `const`, it can be called through `const target&` (`get_initial_variable_data`, `src/target.cpp:81, 89`), which hides that side effect. The returned reference is also invalidated by the next `push_back` in `evaluate_expression`.
Fix: return a fresh value without storing it: `typed_data get_expression_result(std::size_t i) const` re-reads memory into a local and returns it by value. Then remove `mutable` from `expression_results_`. If the stored copy must be updated, make the function non-`const` and call it `refresh_expression_result`.

### 5.4 Expressions & statements

### 70. Inner-scope names shadow outer ones — should
Rules: CG ES.12 “Do not reuse names in nested scopes”; cppbestpractices, ch. 2 "Use the Tools Available", § Compilers → GCC / Clang (recommended warning flags) (recommends `-Wshadow`).
Where:
- `src/type.cpp:47-65`: the parameter `const gsdb::typed_data& data` is shadowed by `auto data = gsdb::typed_data{…}` at `:63`. Line `:60` reads the parameter and line `:65` reads the local.
- `src/target.cpp:326-370`: the outer `for (std::size_t i …)` over parameters is shadowed by the inner `for (std::size_t i = 0; i < param_size; i += 8)` at `:348`. `args[i]` at `:346` is the parameter index, while `param_classes[i / 8]` and `bytes.begin() + i` at `:350` and `:365-367` are byte offsets.
- `src/target.cpp:1003, 1010`: `auto& dwarf` is declared in `find_variable` and again inside its `for_each` lambda.

What:
```cpp
for (std::size_t i = 0; i < params.size(); ++i) {          // parameter index
    ...
        for (std::size_t i = 0; i < param_size; i += 8) {   // byte offset
```
Why: in both function bodies the two meanings sit within a few lines of each other, so a later edit can easily use the wrong one. `-Wshadow` reports every one of these sites.
Fix: rename (`member`, `byte_off`, `elf_dwarf`), then add `-Wshadow` as #61 proposes so new cases fail the build.

### 71. Narrowing, signed/unsigned mixing and `int` masks in the new bit and ABI helpers — should
This continues #30 and #31.
Rules: CG ES.46 “Avoid lossy (narrowing, truncating) arithmetic conversions”; CG ES.100 “Don't mix signed and unsigned arithmetic”; CG ES.101 “Use unsigned types for bit manipulation”; CG I.13 “Do not pass an array as a single pointer”; CG F.24 “Use a `span<T>` or a `span_p<T>` to designate a half-open sequence”.
Where:
- **Bit-field offsets:** `src/dwarf.cpp:2263, 2284, 2288` assign `std::uint64_t` expressions to `std::uint8_t bit_offset`, truncating silently. The struct (`include/libgsdb/dwarf.hpp:589-594`) mixes `std::uint64_t bit_size` with `std::uint8_t bit_offset`.
- **Bit offset narrowed to `int`:** `classify_class_field(…, int bit_offset)` (`src/type.cpp:206-208`) receives `current_bit_offset`, a `std::uint64_t` (`:230`).
- **`strip<int... Tags>`** (`include/libgsdb/type.hpp:49-58`) compares `int` template tags with the `std::uint64_t` DIE tag.
- **`memcpy_bits`** (`include/libgsdb/bit.hpp:55-71`):
  - It builds masks as `int` (`1 << (dest_bit % 8)`) and narrows them to `std::uint8_t`; `~dest_mask` is an `int` again.
  - It takes raw pointers without lengths and `std::uint32_t` bit counts, while callers pass `std::size_t`/`std::uint64_t` values.
  - Callers `reinterpret_cast` `std::byte*` to `std::uint8_t*` to call it (`src/target.cpp:989-992`, `src/dwarf.cpp:2306-2308`).
- **Negative `int` masks:** `(size + 7) & ~7` and `rsp &= ~0xf` (`src/target.cpp:285, 377`) apply negative `int`s to 64-bit unsigned values.

What:
```cpp
std::uint8_t bit_offset = 0;
...
bit_offset = storage_bit_size - offset_field - bit_size;   // uint64_t → uint8_t
```
Why: these belong to the same family as #30 and #31, and they are new code. The bit-field offset truncates once the storage is wider than 255 bits, and the `int` masks depend on sign extension.
Fix:
- Use `std::uint64_t` (or `std::size_t`) for every bit offset and size.
- Write `template <std::uint64_t... Tags>`.
- Change the signature to `memcpy_bits(std::span<std::byte> dest, std::size_t dest_bit, std::span<const std::byte> src, std::size_t src_bit, std::size_t n_bits)` and build masks as `std::byte{1} << n`; the call-site casts then disappear.
- Use `& ~std::uint64_t{7}`, or a named `align_down(rsp, 16)` helper.

### 5.8 Source files & headers

### 72. Header and translation-unit hygiene in the new type layer — consider
Rules: Google C++ Style Guide, “Include What You Use” (covers both the missing `<string>` and the unused `<algorithm>`); CG SF.10 “Avoid dependencies on implicitly `#include`d names”; CG SF.11 “Header files should be self-contained”.
Where:
- `include/libgsdb/type.hpp:4-15` uses `std::string` (`:138`) without including `<string>`. It includes `<algorithm>`, which it does not use.
- `include/libgsdb/target.hpp:13, 36`: `#include <libgsdb/type.hpp>` followed by a redundant `class typed_data;` forward declaration.
- `typed_data::fixup_bitfield` is defined in `src/dwarf.cpp:2293-2313`. No guideline rule covers where a member is defined; this one is about keeping a class's members in its own `.cpp`. It is the only `typed_data` member outside `src/type.cpp`, and its only callers are in `src/type.cpp` (`:63-64`, `:488`).

Fix:
- In `type.hpp`, add `<string>` and drop `<algorithm>`.
- Delete the forward declaration in `target.hpp`.
- Move `fixup_bitfield` into `src/type.cpp`, which already includes `bit.hpp`.

### 5.11 Naming, layout, comments

### 73. clang-format drift in two test targets — consider
Rules: CG NL.4 “Maintain a consistent indentation style”.
Where: `test/targets/overloaded.cpp` (6 replacements) and `test/targets/step.cpp` (3), from `clang-format --dry-run` over every tracked C++ file. Everything else is clean.
Fix: run `clang-format -i test/targets/overloaded.cpp test/targets/step.cpp`. Optionally add a `clang-format --dry-run -Werror` CTest so drift fails the build.

## 6. What is already in good shape

- **The new type layer is type-safe by construction.** `type` holds a `std::variant<die, builtin_type>`, and each accessor throws on the wrong alternative instead of reinterpreting it (CG P.4). "Not found" results consistently use `std::optional` (`find_variable`, `get_bitfield_information`, `get_member_function_definition`).
- **The bug-audit fixes follow the guidelines' own recommendations:**
  - Heterogeneous lookup with a transparent hasher replaces the dangling `string_view` keys (`fd6be79`, ToTW #144).
  - `dwarf(const elf&)` became `explicit` (`501504a`, CG C.46).
  - The `~process` rewrite never throws: it ends in a `catch (...)` and returns early once the process is gone (CG C.36, E.16).
  - The literal-location copy is bounded with `std::min`.
- **Call-site readability improved:** `set_ptrace_options(pid, /*kill_on_tracer_exit=*/true)` is the habit #8 asks for.
- **Internal linkage is consistent:** every new helper in `target.cpp` and `type.cpp` sits in an unnamed namespace (CG SF.22).
- **The why-comments for kernel behaviour are good:** `__WALL`, zombie reaping order, `PTRACE_O_EXITKILL`, the bit-field layout diagram in `get_bitfield_information`, and the SysV eightbyte merge rules.
- **The build is clean again:** every translation unit compiles under `-Werror` (in September, `dwarf.cpp` did not), and the tests now cover variables, DWARF expression pieces and member pointers.

## 7. Proposed `.clang-tidy` for this project

This is the September proposal, still valid, with one addition (`-misc-no-recursion`). The DIE-tree and type-classification walks are recursive by design, and that check produced 23 hits here. Start with `WarningsAsErrors: ''`, clean up the existing warnings, then promote checks to errors.

```yaml
---
Checks: >
  -*,
  cppcoreguidelines-*,
  modernize-*,
  readability-*,
  performance-*,
  misc-*,
  cert-dcl50-cpp, cert-dcl51-cpp, cert-dcl58-cpp, cert-err58-cpp, cert-err60-cpp, cert-err61-cpp,
  cert-msc50-cpp, cert-msc51-cpp, cert-oop54-cpp, cert-oop57-cpp, cert-oop58-cpp,
  google-explicit-constructor, google-build-using-namespace, google-global-names-in-headers,
  google-default-arguments,
  bugprone-easily-swappable-parameters, bugprone-reserved-identifier, bugprone-macro-parentheses,
  -modernize-use-trailing-return-type,
  -readability-identifier-length,
  -readability-magic-numbers, -cppcoreguidelines-avoid-magic-numbers,
  -readability-function-cognitive-complexity,
  -readability-qualified-auto,
  -misc-non-private-member-variables-in-classes, -cppcoreguidelines-non-private-member-variables-in-classes,
  -misc-include-cleaner,
  -misc-no-recursion,
  -cppcoreguidelines-pro-type-vararg,
  -cppcoreguidelines-pro-bounds-pointer-arithmetic,
  -modernize-use-designated-initializers,
  -performance-enum-size
WarningsAsErrors: ''
HeaderFilterRegex: '(include|src|tools|test)/.*'
FormatStyle: file
CheckOptions:
  cppcoreguidelines-special-member-functions.AllowSoleDefaultDtor: true
  cppcoreguidelines-special-member-functions.AllowMissingMoveFunctionsWhenCopyIsDeleted: true
  cppcoreguidelines-macro-usage.AllowedRegexp: '^(GSDB_|DEFINE_(REGISTER|SYSCALL|GPR|FPR))'
  misc-const-correctness.WarnPointersAsValues: false
  readability-implicit-bool-conversion.AllowPointerConditions: true
  readability-operators-representation.BinaryOperators: 'and;or;not'
  readability-operators-representation.OverloadedOperators: 'and;or;not'
  modernize-use-default-member-init.UseAssignment: false
  performance-unnecessary-value-param.AllowedTypes: 'std::string_view;std::span;virt_addr;file_addr;file_offset'
  readability-identifier-naming.NamespaceCase: lower_case
  readability-identifier-naming.ClassCase: lower_case
  readability-identifier-naming.StructCase: lower_case
  readability-identifier-naming.EnumCase: lower_case
  readability-identifier-naming.FunctionCase: lower_case
  readability-identifier-naming.VariableCase: lower_case
  readability-identifier-naming.PrivateMemberSuffix: '_'
  readability-identifier-naming.ProtectedMemberSuffix: '_'
  readability-identifier-naming.MacroDefinitionCase: UPPER_CASE
  readability-identifier-naming.TemplateParameterCase: CamelCase
```

With this configuration, `google-default-arguments`, `performance-unnecessary-value-param` and `performance-for-range-copy` would have flagged most of #65 and #68 automatically. `-Wshadow` from #61 covers #70.

## 8. Suggested order of work

1. **Open musts:** #7 (now 3 lambdas; `[this]` for one and `[]` for the other two), #3 (delete `pipe`'s copy operations) and #1 (RAII members in `elf`). Each is small.
2. **#64:** checking the optionals at 5 sites removes a reachable undefined-behaviour path.
3. **#67:** the `read_cfi_expression` helper fixes bug-audit M1 and the cursor-advance bug together, and `is_user_provided` fixes H11a once instead of twice.
4. **#61 and #70:** add `-Wshadow` and fix the 3 shadowing sites. Then try `-Wconversion` behind an option and use it to work through #71 and #30.
5. **#65:** make the thread explicit inside the library. This prevents the next M7/M11-style bug.
6. **#68 and #9:** a mechanical parameter-passing pass; `clang-tidy --fix` with `performance-*` does most of it.
7. **#69, #66:** two API-shape fixes in `target`.
8. **The remaining should items in §5.0**, then the consider items when you touch those files.

## Appendix A — Grep hotspot details

- **`reinterpret_cast`, 33 lines.** Added since September: `src/target.cpp:129, 940, 989, 991`, `src/dwarf.cpp:2306, 2307`, and one in `test/tests.cpp`, plus two inside commented-out code (`src/target.cpp:960`, `tools/gsdb.cpp:1029`). The older ones in `src/target.cpp` (`:76`, `:814`, `:870-881`) are byte/OS-boundary conversions and are fine. The `uint8_t` casts disappear with #71.
- **`catch (...)`, 3 sites:** `tools/gsdb.cpp:415` (#46), `src/process.cpp:174` (destructor, justified) and `:442` (rethrows after recording state, justified).
- **`mutable` members, 7:** `include/libgsdb/dwarf.hpp:92, 362, 531, 534, 539`; `include/libgsdb/type.hpp:112`; `include/libgsdb/target.hpp:208` (#69).
- **`std::optional<pid_t> otid = std::nullopt`, 22:** 10 in `include/libgsdb/process.hpp` and 12 in `include/libgsdb/target.hpp` (#65).
- **`new T(...)`, 10, all private-constructor factories:** `src/process.cpp:551, 577, 644, 655, 870`; `src/target.cpp:495, 520, 746, 751, 757` (#27).
- **`write_by_id(…, true|false)`, 9:** `src/target.cpp:191, 297, 369, 388, 390, 1121`; `src/process.cpp:1034, 1042`; `src/dwarf.cpp:883` (#8).

## Appendix B — Sources and where to read them

Every source cited in this report, with where to read it. Core Guidelines links go to the rule on the canonical page; book items have no free online text, so the edition and ISBN are given.

**C++ Core Guidelines** — Stroustrup & Sutter (eds.), https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines (snapshot dated Jun 14, 2026)

| Rule | Title |
|---|---|
| [A.2](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#ra-lib) | Express potentially reusable parts as a library |
| [C.9](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rc-private) | Minimize exposure of members |
| [C.12](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rc-constref) | Don't make data members `const` or references in a copyable or movable type |
| [C.21](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rc-five) | If you define or `=delete` any copy, move, or destructor function, define or `=delete` them all |
| [C.31](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rc-dtor-release) | All resources acquired by a class must be released by the class's destructor |
| [C.32](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rc-dtor-ptr) | If a class has a raw pointer (`T*`) or reference (`T&`), consider whether it might be owning |
| [C.36](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rc-dtor-fail) | A destructor must not fail |
| [C.41](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rc-complete) | A constructor should create a fully initialized object |
| [C.46](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rc-explicit) | By default, declare single-argument constructors explicit |
| [C.48](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rc-in-class-initializer) | Prefer default member initializers to member initializers in constructors for constant initializers |
| [C.49](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rc-initialize) | Prefer initialization to assignment in constructors |
| [C.50](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rc-factory) | Use a factory function if you need "virtual behavior" during initialization |
| [C.82](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rc-ctor-virtual) | Don't call virtual functions in constructors and destructors |
| [C.86](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rc-eq) | Make `==` symmetric with respect to operand types and `noexcept` |
| [C.90](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rc-memset) | Rely on constructors and assignment operators, not `memset` and `memcpy` |
| [C.133](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rh-protected) | Avoid `protected` data |
| [C.146](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rh-dynamic_cast) | Use `dynamic_cast` where class hierarchy navigation is unavoidable |
| [C.150](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rh-make_unique) | Use `make_unique()` to construct objects owned by `unique_ptr`s |
| [C.153](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rh-use-virtual) | Prefer virtual function to casting |
| [C.161](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#ro-symmetric) | Use non-member functions for symmetric operators |
| [C.181](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#ru-naked) | Avoid "naked" `union`s |
| [C.182](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#ru-anonymous) | Use anonymous `union`s to implement tagged unions |
| [CP.1](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rconc-multi) | Assume that your code will run as part of a multi-threaded program |
| [CPL.3](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rcpl-interface) | If you must use C for interfaces, use C++ in the calling code using such interfaces |
| [Con.2](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rconst-fct) | By default, make member functions `const` |
| [Con.3](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rconst-ref) | By default, pass pointers and references to `const`s |
| [Con.4](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rconst-const) | Use `const` to define objects with values that do not change after construction |
| [Con.5](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rconst-constexpr) | Use `constexpr` for values that can be computed at compile time |
| [E.1](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#re-design) | Develop an error-handling strategy early in a design |
| [E.2](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#re-throw) | Throw an exception to signal that a function can't perform its assigned task |
| [E.6](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#re-raii) | Use RAII to prevent leaks |
| [E.14](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#re-exception-types) | Use purpose-designed user-defined types as exceptions (not built-in types) |
| [E.15](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#re-exception-ref) | Throw by value, catch exceptions from a hierarchy by reference |
| [E.17](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#re-not-always) | Don't try to catch every exception in every function |
| [E.19](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#re-finally) | Use a `final_action` object to express cleanup if no suitable resource handle is available |
| [E.25](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#re-no-throw-raii) | If you can't throw exceptions, simulate RAII for resource management |
| [E.28](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#re-no-throw) | Avoid error handling based on global state (e.g. `errno`) |
| [ES.1](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-lib) | Prefer the standard library to other libraries and to "handcrafted code" |
| [ES.3](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-dry) | Don't repeat yourself, avoid redundant code |
| [ES.6](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-cond) | Declare names in for-statement initializers and conditions to limit scope |
| [ES.10](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-name-one) | Declare one name (only) per declaration |
| [ES.12](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-reuse) | Do not reuse names in nested scopes |
| [ES.20](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-always) | Always initialize an object |
| [ES.22](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-init) | Don't declare a variable until you have a value to initialize it with |
| [ES.27](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-stack) | Use `std::array` or `stack_array` for arrays on the stack |
| [ES.28](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-lambda-init) | Use lambdas for complex initialization, especially of `const` variables |
| [ES.31](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-macros2) | Don't use macros for constants or "functions" |
| [ES.32](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-all_caps) | Use `ALL_CAPS` for all macro names |
| [ES.33](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-macros3) | If you must use macros, give them unique names |
| [ES.41](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-parens) | If in doubt about operator precedence, parenthesize |
| [ES.45](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-magic) | Avoid "magic constants"; use symbolic constants |
| [ES.46](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-narrowing) | Avoid lossy (narrowing, truncating) arithmetic conversions |
| [ES.48](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-casts) | Avoid casts |
| [ES.49](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-casts-named) | If you must use a cast, use a named cast |
| [ES.50](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-casts-const) | Don't cast away `const` |
| [ES.56](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-move) | Write `std::move()` only when you need to explicitly move an object to another scope |
| [ES.65](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-deref) | Don't dereference an invalid pointer |
| [ES.71](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-for-range) | Prefer a range-`for`-statement to a `for`-statement when there is a choice |
| [ES.73](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-while-for) | Prefer a `while`-statement to a `for`-statement when there is no obvious loop variable |
| [ES.75](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-do) | Avoid `do`-statements |
| [ES.78](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-break) | Don't rely on implicit fallthrough in `switch` statements |
| [ES.87](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-if) | Don't add redundant `==` or `!=` to conditions |
| [ES.100](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-mix) | Don't mix signed and unsigned arithmetic |
| [ES.101](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-unsigned) | Use unsigned types for bit manipulation |
| [ES.102](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-signed) | Use signed types for arithmetic |
| [ES.104](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-underflow) | Don't underflow |
| [ES.106](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#res-nonnegative) | Don't try to avoid negative values by using `unsigned` |
| [Enum.2](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#renum-set) | Use enumerations to represent sets of related named constants |
| [Enum.3](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#renum-class) | Prefer class enums over "plain" enums |
| [Enum.4](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#renum-oper) | Define operations on enumerations for safe and simple use |
| [Enum.6](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#renum-unnamed) | Avoid unnamed enumerations |
| [F.1](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rf-package) | "Package" meaningful operations as carefully named functions |
| [F.2](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rf-logical) | A function should perform a single logical operation |
| [F.7](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rf-smart) | For general use, take `T*` or `T&` arguments rather than smart pointers |
| [F.9](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rf-unused) | Unused parameters should be unnamed |
| [F.16](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rf-in) | For "in" parameters, pass cheaply-copied types by value and others by reference to `const` |
| [F.18](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rf-consume) | For "will-move-from" parameters, pass by `X&&` and `std::move` the parameter |
| [F.24](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rf-range) | Use a `span<T>` or a `span_p<T>` to designate a half-open sequence |
| [F.49](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rf-return-const) | Don't return `const T` |
| [F.53](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rf-value-capture) | Avoid capturing by reference in lambdas that will be used non-locally, including returned, stored on the heap, or passed to another thread |
| [F.54](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rf-this-capture) | When writing a lambda that captures `this` or any class data member, don't use `[=]` default capture |
| [F.60](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rf-ptr-ref) | Prefer `T*` over `T&` when "no argument" is a valid option |
| [I.1](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#ri-explicit) | Make interfaces explicit |
| [I.2](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#ri-global) | Avoid non-`const` global variables |
| [I.4](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#ri-typed) | Make interfaces precisely and strongly typed |
| [I.5](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#ri-pre) | State preconditions (if any) |
| [I.6](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#ri-expects) | Prefer `Expects()` for expressing preconditions |
| [I.9](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#ri-concepts) | If an interface is a template, document its parameters using concepts |
| [I.13](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#ri-array) | Do not pass an array as a single pointer |
| [I.24](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#ri-unrelated) | Avoid adjacent parameters that can be invoked by the same arguments in either order with different meaning |
| [NL.1](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rl-comments) | Don't say in comments what can be clearly stated in code |
| [NL.2](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rl-comments-intent) | State intent in comments |
| [NL.3](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rl-comments-crisp) | Keep comments crisp |
| [NL.4](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rl-indent) | Maintain a consistent indentation style |
| [NL.8](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rl-name) | Use a consistent naming style |
| [NL.16](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rl-order) | Use a conventional class member declaration order |
| [NL.19](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rl-misread) | Avoid names that are easily misread |
| [NL.25](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rl-void) | Don't use `void` as an argument type |
| [NL.27](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rl-file-suffix) | Use a `.cpp` suffix for code files and `.h` for interface files |
| [P.2](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rp-cplusplus) | Write in ISO Standard C++ |
| [P.4](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rp-typesafe) | Ideally, a program should be statically type safe |
| [P.9](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rp-waste) | Don't waste time or space |
| [P.12](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rp-tools) | Use supporting tools as appropriate |
| [Pro.lifetime](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#ss-lifetime) | Lifetime safety profile |
| [R.1](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rr-raii) | Manage resources automatically using resource handles and RAII (Resource Acquisition Is Initialization) |
| [R.3](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rr-ptr) | A raw pointer (a `T*`) is non-owning |
| [R.10](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rr-mallocfree) | Avoid `malloc()` and `free()` |
| [R.12](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rr-immediate-alloc) | Immediately give the result of an explicit resource allocation to a manager object |
| [R.30](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rr-smartptrparam) | Take smart pointers as parameters only to explicitly express lifetime semantics |
| [SF.1](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rs-file-suffix) | Use a `.cpp` suffix for code files and `.h` for interface files if your project doesn't already follow another convention |
| [SF.9](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rs-cycles) | Avoid cyclic dependencies among source files |
| [SF.10](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rs-implicit) | Avoid dependencies on implicitly `#include`d names |
| [SF.11](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rs-contained) | Header files should be self-contained |
| [SF.12](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rs-incform) | Prefer the quoted form of `#include` for files relative to the including file and the angle bracket form everywhere else |
| [SF.22](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rs-unnamed2) | Use an unnamed (anonymous) namespace for all internal/non-exported entities |
| [SL.4](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#sl-safe) | Use the standard library in a type-safe manner |
| [SL.con.1](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rsl-arrays) | Prefer using STL `array` or `vector` instead of a C array |
| [SL.con.4](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rsl-copy) | don't use `memset` or `memcpy` for arguments that are not trivially-copyable |
| [SL.io.1](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rio-low) | Use character-level input only when you have to |
| [SL.io.2](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rio-validate) | When reading, always consider ill-formed input |
| [SL.io.3](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rio-streams) | Prefer `iostream`s for I/O |
| [SL.io.50](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rio-endl) | Avoid `endl` |
| [SL.str.1](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rstr-string) | Use `std::string` to own character sequences |
| [SL.str.2](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rstr-view) | Use `std::string_view` or `gsl::span<char>` to refer to character sequences |
| [T.10](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rt-concepts) | Specify concepts for all template arguments |
| [T.11](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rt-std-concepts) | Whenever possible use standard concepts |
| [T.47](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rt-visible) | Avoid highly visible unconstrained templates with common names |
| [T.144](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rt-specialize-function) | Don't specialize function templates |

**Scott Meyers, *Effective Modern C++* (O'Reilly, 2014), ISBN 978-1-491-90399-5** — author's page: https://www.aristeia.com/books.html

- Item 10: Prefer scoped enums to unscoped enums (ch. 3)
- Item 15: Use constexpr whenever possible (ch. 3)
- Item 16: Make const member functions thread safe (ch. 3)
- Item 17: Understand special member function generation (ch. 3)
- Item 21: Prefer std::make_unique and std::make_shared to direct use of new (ch. 4)
- Item 31: Avoid default capture modes (ch. 6)
- Item 41: Consider pass by value for copyable parameters that are cheap to move and always copied (ch. 8)

**Scott Meyers, *Effective C++*, 3rd ed. (Addison-Wesley, 2005), ISBN 978-0-321-33487-9** — author's page: https://www.aristeia.com/books.html

- Item 3: Use const whenever possible
- Item 4: Make sure that objects are initialized before they're used
- Item 6: Explicitly disallow the use of compiler-generated functions you do not want
- Item 9: Never call virtual functions during construction or destruction
- Item 13: Use objects to manage resources
- Item 28: Avoid returning "handles" to object internals
- Item 31: Minimize compilation dependencies between files
- Item 53: Pay attention to compiler warnings

**Scott Meyers, *Effective STL* (Addison-Wesley, 2001), ISBN 978-0-201-74962-5** — author's page: https://www.aristeia.com/books.html

- Item 4: Call empty instead of checking size() against zero
- Item 45: Distinguish among count, find, binary_search, lower_bound, upper_bound, and equal_range

**Herb Sutter & Andrei Alexandrescu, *C++ Coding Standards: 101 Rules, Guidelines, and Best Practices* (Addison-Wesley, 2004), ISBN 978-0-321-11358-0**

- Item 0: Don't sweat the small stuff. (Or: Know what not to standardize.)
- Item 1: Compile cleanly at high warning levels
- Item 2: Use an automated build system
- Item 5: Give one entity one cohesive responsibility
- Item 13: Ensure resources are owned by objects. Use explicit RAII and smart pointers
- Item 17: Avoid magic numbers
- Item 18: Declare variables as locally as possible
- Item 19: Always initialize variables
- Item 22: Minimize definitional dependencies. Avoid cyclic dependencies
- Item 41: Make data members private, except in behaviorless aggregates (C-style structs)
- Item 48: In constructors, prefer initialization to assignment
- Item 52: Copy and destroy consistently
- Item 53: Explicitly enable or disable copying
- Item 66: Don't specialize function templates
- Item 90: Avoid type switching; prefer polymorphism
- Item 91: Rely on types, not on representations
- Item 92: Avoid using reinterpret_cast

**Abseil C++ Tips of the Week** — https://abseil.io/tips/

- [#77: Temporaries, Moves, and Copies](https://abseil.io/tips/77)
- [#94: Callsite Readability and bool Parameters](https://abseil.io/tips/94)
- [#117: Copy Elision and Pass-by-value](https://abseil.io/tips/117)
- [#134: make_unique and private Constructors](https://abseil.io/tips/134)
- [#141: Beware Implicit Conversions to bool](https://abseil.io/tips/141)
- [#144: Heterogeneous Lookup in Associative Containers](https://abseil.io/tips/144)
- [#163: Passing std::optional parameters](https://abseil.io/tips/163)
- [#188: Be Careful With Smart-Pointer Function Parameters](https://abseil.io/tips/188)

**Google C++ Style Guide** — https://google.github.io/styleguide/cppguide.html

- [Default Arguments](https://google.github.io/styleguide/cppguide.html#Default_Arguments)
- [Function Names](https://google.github.io/styleguide/cppguide.html#Function_Names)
- [Implicit Conversions](https://google.github.io/styleguide/cppguide.html#Implicit_Conversions)
- [Include What You Use](https://google.github.io/styleguide/cppguide.html#Include_What_You_Use)
- [Names and Order of Includes](https://google.github.io/styleguide/cppguide.html#Names_and_Order_of_Includes)
- [The #define Guard](https://google.github.io/styleguide/cppguide.html#The__define_Guard)

**LLVM Coding Standards** — https://llvm.org/docs/CodingStandards.html

- [Comment Formatting ("Commenting out large blocks of code is discouraged")](https://llvm.org/docs/CodingStandards.html#comment-formatting)
- [Don't use else after a return](https://llvm.org/docs/CodingStandards.html#don-t-use-else-after-a-return)

**SEI CERT C and C++ Coding Standards** — https://cmu-sei.github.io/secure-coding-standards/ (C rules apply to the C library calls gsdb makes)

- [ERR30-C: Take care when reading errno](https://cmu-sei.github.io/secure-coding-standards/sei-cert-c-coding-standard/rules/error-handling-err/err30-c)
- [ERR50-CPP: Do not abruptly terminate the program](https://cmu-sei.github.io/secure-coding-standards/sei-cert-cpp-coding-standard/rules/exceptions-and-error-handling-err/err50-cpp)
- [ERR62-CPP: Detect errors when converting a string to a number](https://cmu-sei.github.io/secure-coding-standards/sei-cert-cpp-coding-standard/rules/exceptions-and-error-handling-err/err62-cpp)
- [FIO51-CPP: Close files when they are no longer needed](https://cmu-sei.github.io/secure-coding-standards/sei-cert-cpp-coding-standard/rules/input-output-fio/fio51-cpp)
- [MEM50-CPP: Do not access freed memory](https://cmu-sei.github.io/secure-coding-standards/sei-cert-cpp-coding-standard/rules/memory-management-mem/mem50-cpp)
- [MSC12-C: Detect and remove code that has no effect or is never executed](https://cmu-sei.github.io/secure-coding-standards/sei-cert-c-coding-standard/recommendations/miscellaneous-msc/msc12-c)
- [MSC13-C: Detect and remove unused values](https://cmu-sei.github.io/secure-coding-standards/sei-cert-c-coding-standard/recommendations/miscellaneous-msc/msc13-c)
- [MSC54-CPP: A signal handler must be a plain old function](https://cmu-sei.github.io/secure-coding-standards/sei-cert-cpp-coding-standard/rules/miscellaneous-msc/msc54-cpp)
- [OOP50-CPP: Do not invoke virtual functions from constructors or destructors](https://cmu-sei.github.io/secure-coding-standards/sei-cert-cpp-coding-standard/rules/object-oriented-programming-oop/oop50-cpp)
- [POS54-C: Detect and handle POSIX library errors](https://cmu-sei.github.io/secure-coding-standards/sei-cert-c-coding-standard/rules/posix-pos/pos54-c)
- [SIG30-C: Call only asynchronous-safe functions within signal handlers](https://cmu-sei.github.io/secure-coding-standards/sei-cert-c-coding-standard/rules/signals-sig/sig30-c)
- [SIG31-C: Do not access shared objects in signal handlers](https://cmu-sei.github.io/secure-coding-standards/sei-cert-c-coding-standard/rules/signals-sig/sig31-c)
- [STR37-C: Arguments to character-handling functions must be representable as an unsigned char](https://cmu-sei.github.io/secure-coding-standards/sei-cert-c-coding-standard/rules/characters-and-strings-str/str37-c)

**Jason Turner et al., *C++ Best Practices*** — https://github.com/cpp-best-practices/cppbestpractices

- [cppbestpractices, ch. 2 "Use the Tools Available", §§ LLVM-based tools / Static Analyzers](https://github.com/cpp-best-practices/cppbestpractices/blob/master/02-Use_the_Tools_Available.md#static-analyzers)
- [cppbestpractices, ch. 2 "Use the Tools Available", § codespell](https://github.com/cpp-best-practices/cppbestpractices/blob/master/02-Use_the_Tools_Available.md#codespell)
- [cppbestpractices, ch. 8 "Considering Performance", § Get rid of std::endl](https://github.com/cpp-best-practices/cppbestpractices/blob/master/08-Considering_Performance.md#get-rid-of-stdendl)
- [cppbestpractices, ch. 8 "Considering Performance", § Forward Declare When Possible](https://github.com/cpp-best-practices/cppbestpractices/blob/master/08-Considering_Performance.md#forward-declare-when-possible)
- [cppbestpractices, ch. 2 "Use the Tools Available", § Compilers → GCC / Clang (recommended warning flags)](https://github.com/cpp-best-practices/cppbestpractices/blob/master/02-Use_the_Tools_Available.md#gcc--clang)

**Reference documentation and articles**

- [cppreference: Alternative operator representations (`and`, `or`, `not`)](https://en.cppreference.com/w/cpp/language/operator_alternative)
- [cppreference: `std::as_bytes`](https://en.cppreference.com/w/cpp/container/span/as_bytes)
- [cppreference: C++ library headers, "C compatibility headers" (`<cstdint>` guarantees `std::uint64_t`; the global `::uint64_t` is only "may also")](https://en.cppreference.com/w/cpp/header)
- [CMake docs: `CMAKE_<LANG>_COMPILER` (set in a toolchain file or with `-D`)](https://cmake.org/cmake/help/latest/variable/CMAKE_LANG_COMPILER.html)
- [CMake docs: `include_directories()` ("Prefer the `target_include_directories()` command")](https://cmake.org/cmake/help/latest/command/include_directories.html)
- [CMake docs: `cmake-presets(7)`](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html)
- [cppreference: associative-container `contains` (C++20)](https://en.cppreference.com/w/cpp/container/unordered_multimap/contains)
- [cppreference: `std::countr_zero` (C++20)](https://en.cppreference.com/w/cpp/numeric/countr_zero)
- [cppreference: Explicit object member functions ("deducing `this`", C++23)](https://en.cppreference.com/w/cpp/language/member_functions#Explicit_object_member_functions)
- [cppreference: Default comparisons (C++20; `!=` is rewritten from `==`, defaulted `<=>`)](https://en.cppreference.com/w/cpp/language/default_comparisons)
- [cppreference: `EXIT_SUCCESS`, `EXIT_FAILURE`](https://en.cppreference.com/w/cpp/utility/program/EXIT_status)
- [Herb Sutter, "Why Not Specialize Function Templates?" (C/C++ Users Journal, 2001)](http://www.gotw.ca/publications/mill17.htm)
- [cppreference: `std::ignore`](https://en.cppreference.com/w/cpp/utility/tuple/ignore)
- [cppreference: `std::isdigit` (UB unless the argument is representable as `unsigned char`)](https://en.cppreference.com/w/cpp/string/byte/isdigit)
- [cppreference: `[[maybe_unused]]` attribute](https://en.cppreference.com/w/cpp/language/attributes/maybe_unused)
- [Henry Schreiner et al., *An Introduction to Modern CMake*](https://cliutils.gitlab.io/modern-cmake/)
- [cppreference: `[[nodiscard]]` attribute](https://en.cppreference.com/w/cpp/language/attributes/nodiscard)
- [POSIX.1-2024 `fork()`: in a multi-threaded process the child may call only async-signal-safe functions until `exec` (`_exit` is one, `exit` is not)](https://pubs.opengroup.org/onlinepubs/9799919799/functions/fork.html)
- [cppreference: `std::println` (C++23)](https://en.cppreference.com/w/cpp/io/println)
- [cppreference: `std::ranges::to` (C++23)](https://en.cppreference.com/w/cpp/ranges/to)
- [cppreference: `std::span` (C++20)](https://en.cppreference.com/w/cpp/container/span)
- [cppreference: `std::views::split`](https://en.cppreference.com/w/cpp/ranges/split_view)
- [cppreference: `std::basic_string::starts_with` (C++20)](https://en.cppreference.com/w/cpp/string/basic_string/starts_with)
- [clang-tidy: `modernize-use-nodiscard`](https://clang.llvm.org/extra/clang-tidy/checks/modernize/use-nodiscard.html)
- [clang-tidy: `readability-operators-representation`](https://clang.llvm.org/extra/clang-tidy/checks/readability/operators-representation.html)
- [cppreference: `std::to_underlying` (C++23)](https://en.cppreference.com/w/cpp/utility/to_underlying)
- [cppreference: `std::unreachable` (C++23)](https://en.cppreference.com/w/cpp/utility/unreachable)
