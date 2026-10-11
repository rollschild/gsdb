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
- Other sources cited: Abseil Tip of the Week (ToTW), the Google C++ Style Guide, and CERT C/C++.

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

| # | Sev | Title (short) | Status | New sites since `4915d55` → Fix |
|---|---|---|---|---|
| 1 | must | `elf` ctor without RAII members | open | — |
| 2 | must | `symbol_name_map_` keys dangle | **fixed** (`fd6be79`) | — |
| 3 | must | `pipe` copyable | open | — |
| 4 | should | Manual C resource pairs | open | `test/tests.cpp` "Local variables" (`open("/dev/null")`/`close`). Fix: a test-local `unique_fd`. |
| 5 | should | `file_entry` into a growable `mutable` vector | open | — |
| 6 | consider | `optional<T*>` returns | open | — |
| 7 | must | Stored callback captures `[&]` | open, **+2** | `src/target.cpp:1117-1119`, `:1147-1149` (`install_hit_handler([&] { return false; })`). Fix: `[]`, since nothing is used. |
| 8 | should | `bool` params / bare literals | open, **+16** | New `bool` params: `attr::as_expression`, `as_location_list`, `as_evaluated_location(…, bool in_frame_info)` (`include/libgsdb/dwarf.hpp:257, 261, 266`); `dwarf::index_die(…, bool in_function = false)` (`:515`). Bare literals: `as_evaluated_location(…, false)` (`src/target.cpp:98-99`; `tools/gsdb.cpp:938-940, 971-972`); `write_by_id(…, true)` ×9 (`src/target.cpp:191, 297, 369, 388, 390, 1121`; `src/process.cpp:1034, 1042`; `src/dwarf.cpp:883` with `false`). Fix: `enum class frame_info { no, yes }` and `enum class commit { no, yes }`, or `/*in_frame_info=*/false` at call sites, the way `set_ptrace_options(…, /*kill_on_tracer_exit=*/true)` already does. |
| 9 | should | Parameter passing | open, **+many** | → #68 |
| 10 | should | `unique_ptr<target>&` params | open | — |
| 11 | should | Non-`explicit` single-arg ctors | **partly fixed** (`dwarf`, `501504a`), **+2** | `type(die)`, `type(builtin_type)` (`include/libgsdb/type.hpp:34-35`). `strip()` depends on the implicit `die → type` conversion through a round trip, `ret = ret.get_die()[DW_AT_type].as_type().get_die();` (`:59`). Fix: make `type(die)` `explicit` and write `ret = ret.get_die()[DW_AT_type].as_type();`. Keeping `type(builtin_type)` implicit for literal arguments is defensible, but say so in a comment. |
| 12 | should | Unused params, stale `[[maybe_unused]]`, dead locals | open, **+4** | `[[maybe_unused]] auto bit_size` (`src/type.cpp:213`); `[[maybe_unused]] auto param_classes` (`src/target.cpp:304`), which runs the whole classification and throws the result away; `[[maybe_unused]] auto& dwarf` (`src/target.cpp:1035`); `[[maybe_unused]] const gsdb::process& proc` (`src/dwarf.cpp:2294`). Fix: delete the locals and leave the parameter unnamed. |
| 13 | should | `up()`/`down()` preconditions | open | — |
| 14 | should | Small API-shape issues | open | — |
| 15 | consider | `[[nodiscard]]` | open | `find_variable`, `resolve_indirect_name`, `evaluate_expression`, `read_location_data`, `inferior_malloc`. |
| 16 | should | Read-before-write members | open | — |
| 17 | should | Assign in ctor body | open | — |
| 18 | should | `breakpoint` protected data and duplication | open | — |
| 19 | should | Virtual `resolve()` from ctors | open | — |
| 20 | should | `dynamic_cast` chain | open | — |
| 21 | should | Const-correctness holes | open, **+1** | `std::array<gsdb::register_id, 6> int_regs` and `sse_regs` are rebuilt on every call (`src/target.cpp:260-270`). Fix: `static constexpr std::array`. |
| 22 | should | `const_cast` const-overload idiom | open | — |
| 23 | should | Comparison operators | open, **+1** | `type::operator!=` is hand-written next to `operator==` (`include/libgsdb/type.hpp:96-97`). C++20 rewrites `!=` from `==`. Fix: delete `operator!=`. |
| 24 | should | `eh_hdr::parent` unused | open | — |
| 25 | consider | Anonymous union | open | — |
| 26 | consider | `process` responsibilities | open | It gained `read_string` and `inferior_call` (`include/libgsdb/process.hpp:252-263`). |
| 27 | consider | `unique_ptr<T>(new T)` factories | open (note only) | — |
| 28 | should | Declare-then-assign | open, **+4** | `gsdb::register_id reg;` before a `switch` (`src/target.cpp:349`); `gsdb::virt_addr call_addr;` before an `if`/`else` (`:453`); `std::uint64_t cfa;` (`src/dwarf.cpp:869`); `std::vector<std::byte> fixed_data; fixed_data.resize(n);` (`:2303-2304`). Fix: an immediately-invoked lambda or a `?:` expression, and `std::vector<std::byte> fixed_data(storage_byte_size);`. |
| 29 | should | Magic constants | open, **+8** | 8/16-byte sizes (`src/type.cpp:366-371, 378, 388-390`); `(size + 7) & ~7` and `rsp &= ~0xf` (`src/target.cpp:285, 377`); eightbyte stride `8` (`:348-366`); `4096` (`:879`); `1024` (`src/process.cpp:1011`); `rsp -= 8` (`:1038`). Fix: `inline constexpr std::size_t eightbyte = 8;`, `stack_alignment = 16`, `pointer_size = 8`, `linux_path_max = 4096`. |
| 30 | should | Signed/unsigned in address math | open | → #71 for the new code |
| 31 | should | Bit manipulation on signed `int` | open | → #71 for the new code |
| 32 | should | Avoidable `reinterpret_cast`s | open, **+7** | `src/target.cpp:129, 940, 989, 991`; `src/dwarf.cpp:2306-2307`; `test/tests.cpp` (DWARF expression test). Two more sit in commented-out code (#34). Fix: see #71 (span-based `memcpy_bits`) and #67 (`to_byte_vec`). |
| 33 | should | Names that shadow type names | open, **+6** | Locals named `die` or `type` that shadow the classes (`src/type.cpp:118-119, 424, 466, 615, 746`); `type(die die)` and `type(builtin_type type)` (`include/libgsdb/type.hpp:34-35`). Fix: `d`, `ty`, `type_die`, and so on. |
| 34 | should | Commented-out code | open, **+2** | `src/target.cpp:959-964`; `tools/gsdb.cpp:1020-1031`. Fix: delete. |
| 35 | should | Two spellings of logical operators | open | — |
| 36 | consider | Loop shapes | open | — |
| 37 | consider | Readability nits | open, **+8** | `case X: error::send(...);` followed by the next label, inside `visualize_base_type` (`src/type.cpp:135-136, 151-153, 168-170, 175-176`); `else` after `return` (`src/type.cpp:510`, `src/target.cpp:133, 149, 945`, `src/dwarf.cpp:2226`); `if (…) return true; return false;` (`src/type.cpp:579-583`). Fix: as in #37. |
| 38 | should | Enum arithmetic for debug registers | open | — |
| 39 | should | X-macro helper macros leak | open | — |
| 40 | consider | Enum and constant hygiene | open | — |
| 41 | should | Unconstrained templates | open, **+2** | `to_byte_span<From>` and `to_byte_vec<From>` (`include/libgsdb/bit.hpp:73-83`) `memcpy` any type. Fix: `requires std::is_trivially_copyable_v<From>`. |
| 42 | should | Function template specialization | open | — |
| 43 | should | Hand-rolled `gsdb::span` | open | — |
| 44 | should | `errno` read late | open | — |
| 45 | should | Library prints and terminates | open | — |
| 46 | should | `catch` by non-const ref, catch-all | open | — |
| 47 | should | Unchecked external input | open, **+1** | `std::isdigit(arg[0])` on a plain `char` (`src/target.cpp:141`). Fix: `std::isdigit(static_cast<unsigned char>(arg[0]))`. |
| 48 | consider | Error-policy consistency | open, **+2** | New deliberate catch-alls: `target::notify_stop` (`src/target.cpp:536-541`) and `~process` (`src/process.cpp:174-175`). Both are justified (best-effort unwind; CG C.36). Fix: one comment on each saying why. |
| 49 | should | Mixed include styles | open | `type.hpp` and `type.cpp` repeat the angle/quoted split. |
| 50 | should | Heavy includes | open, **+1** | `target.hpp` now includes `type.hpp` (needed: `std::optional<typed_data>` member). See #72. |
| 51 | consider | Header naming, C headers | open | — |
| 52 | should | `count()` then `at()` | open, **+1** | `!seen.count(name)` (`tools/gsdb.cpp:937`). Fix: `contains`. |
| 53 | should | Output mechanisms | open, **+2** | `std::cerr << "Variable not found!";` with no newline, next to `std::print` (`tools/gsdb.cpp:967`); `std::print("None")` with no newline (`:984`). Fix: `std::println(stderr, …)` and `std::println`. |
| 54 | should | `__builtin_ctzll` | open | — |
| 55 | consider | Standard-library idiom nits | open, **+6** | `arg.find(".") != npos` (`src/target.cpp:142`) → `arg.contains('.')`; `std::reverse`/`std::find_if`/`std::count` with `begin()`/`end()` (`src/type.cpp:112, 468`; `src/target.cpp:333-337`) → `std::ranges::*`; `push_back(std::pair{&elf, sym})` (`src/target.cpp:732`) → `emplace_back(&elf, sym)`; `insert(std::make_pair(…))` (`src/dwarf.cpp:1437, 1441`) → `emplace`; `using namespace gsdb;` in a function that still qualifies half its names (`src/type.cpp:183-203`) → `using enum gsdb::parameter_class;`. |
| 56 | should | SIGINT handler through a global | open | — |
| 57 | consider | Document single-threaded caches | open, **+3** | `type::byte_size_` (`include/libgsdb/type.hpp:112`), `dwarf::global_variable_index_` and `member_function_index_` (`include/libgsdb/dwarf.hpp:534, 539`). `expression_results_` is not a cache; see #69. |
| 58 | should | `get_x()` vs `x()` accessors | open | New `get_die`, `get_builtin_type`, `get_bitfield_information`, `get_member_function_definition` next to `value_type()`, `address()`, `data()`. |
| 59 | should | Tutorial and stale comments | open, **+6** | Tutorial: parameter pack and fold expression (`include/libgsdb/type.hpp:49-58`), "mutable because…" (`:110-111`), `~dest_mask` (`include/libgsdb/bit.hpp:64`). Stale: "into an `sdb::typed_data` object" (`src/target.cpp:115`); `// TODO` above code that already does the job (`:124`); "demangle it and return it" when it no longer demangles (`:777-779`). Fix: as in #59. |
| 60 | consider | Identifier spelling and layout | open, **+5** | Local `die_` with the member suffix (`src/type.cpp:374`); comment typos `pionts` (`src/type.cpp:564`), `direcly` (`include/libgsdb/elf.hpp:105`), `currect` (`src/target.cpp:1095`), `share lib` (`:888`). |
| 61 | should | 2005-era warning set | open | `-Wshadow` would have caught all of #70. |
| 62 | should | No `.clang-tidy`; dead GTest | open | — |
| 63 | consider | CMake modernization | open | — |

### 5.1 Ownership & lifetime

### 64. `std::optional` dereferenced without a check — should
Rules: CG ES.65 "Don't dereference an invalid pointer"; CG SL.4 "Use the standard library in a type-safe manner"; CG I.5 "State preconditions (if any)".
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
Rules: CG I.1 "Make interfaces explicit"; CG F.51 "Where there is a choice, prefer default arguments over overloading" (defaults are for "the same operation with fewer arguments", not a different subject); Google:Default Arguments.
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
Rules: CG ES.3 "Don't repeat yourself, avoid redundant code"; CG F.1 "'Package' meaningful operations as carefully named functions"; CG A.2 "Express potentially reusable parts as a library"; `CLAUDE.md` ("move debugger primitives out of `tools/gsdb.cpp` into `libgsdb`").
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
Rules: CG ES.3 "Don't repeat yourself, avoid redundant code"; CG F.1 "'Package' meaningful operations as carefully named functions".
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
Rules: CG F.16 "For 'in' parameters, pass cheaply-copied types by value and others by reference to `const`"; CG F.18 "For 'will-move-from' parameters, pass by `X&&` and `std::move` the parameter"; CG P.9 "Don't waste time or space"; EMC++ #41; ToTW #77, #117.
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
Rules: CG Con.2 "By default, make member functions `const`" (a function should be `const` unless it changes observable state; `mutable` is for caching and memoization, per the example under CG ES.50 "Don't cast away `const`"); EMC++ #16.
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
Rules: CG ES.12 "Do not reuse names in nested scopes"; see also #61 (`-Wshadow`).
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
Rules: CG ES.46 "Avoid lossy (narrowing, truncating) arithmetic conversions"; CG ES.100 "Don't mix signed and unsigned arithmetic"; CG ES.101 "Use unsigned types for bit manipulation"; CG I.13 "Do not pass an array as a single pointer"; CG F.24 "Use a `span<T>` or a `span_p<T>` to designate a half-open sequence".
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
Rules: CG SF.10 "Avoid dependencies on implicitly `#include`d names"; CG SF.11 "Header files should be self-contained"; CG SF.5 "A `.cpp` file must include the header file(s) that defines its interface".
Where:
- `include/libgsdb/type.hpp:4-15` uses `std::string` (`:138`) without including `<string>`. It includes `<algorithm>`, which it does not use.
- `include/libgsdb/target.hpp:13, 36`: `#include <libgsdb/type.hpp>` followed by a redundant `class typed_data;` forward declaration.
- `typed_data::fixup_bitfield` is defined in `src/dwarf.cpp:2293-2313`. It is the only `typed_data` member outside `src/type.cpp`, and its only callers are in `src/type.cpp` (`:63-64`, `:488`).

Fix:
- In `type.hpp`, add `<string>` and drop `<algorithm>`.
- Delete the forward declaration in `target.hpp`.
- Move `fixup_bitfield` into `src/type.cpp`, which already includes `bit.hpp`.

### 5.11 Naming, layout, comments

### 73. clang-format drift in two test targets — consider
Rules: CG NL.4 "Maintain a consistent indentation style".
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

## Appendix B — Rules referenced in this report

- A.2 Express potentially reusable parts as a library
- C.36 A destructor must not fail
- C.46 By default, declare single-argument constructors explicit
- Con.2 By default, make member functions `const`
- E.16 Destructors, deallocation, `swap`, and exception type copy/move construction must never fail
- ES.3 Don't repeat yourself, avoid redundant code
- ES.12 Do not reuse names in nested scopes
- ES.46 Avoid lossy (narrowing, truncating) arithmetic conversions
- ES.50 Don't cast away `const`
- ES.65 Don't dereference an invalid pointer
- ES.100 Don't mix signed and unsigned arithmetic
- ES.101 Use unsigned types for bit manipulation
- F.1 "Package" meaningful operations as carefully named functions
- F.16 For "in" parameters, pass cheaply-copied types by value and others by reference to `const`
- F.18 For "will-move-from" parameters, pass by `X&&` and `std::move` the parameter
- F.24 Use a `span<T>` or a `span_p<T>` to designate a half-open sequence
- F.51 Where there is a choice, prefer default arguments over overloading
- I.1 Make interfaces explicit
- I.5 State preconditions (if any)
- I.13 Do not pass an array as a single pointer
- NL.4 Maintain a consistent indentation style
- P.4 Ideally, a program should be statically type safe
- P.9 Don't waste time or space
- SF.5 A `.cpp` file must include the header file(s) that defines its interface
- SF.10 Avoid dependencies on implicitly `#include`d names
- SF.11 Header files should be self-contained
- SF.22 Use an unnamed (anonymous) namespace for all internal/non-exported entities
- SL.4 Use the standard library in a type-safe manner
- EMC++ #16 Make const member functions thread safe; EMC++ #41 Consider pass by value for copyable parameters that are cheap to move and always copied
- ToTW #77 Temporaries, moves, and copies; #117 Copy elision and pass-by-value; #144 Heterogeneous lookup
- Google: Default Arguments
- The rules cited in the §5.0 status rows are listed in Appendix B of the 2026-09-06 report.
