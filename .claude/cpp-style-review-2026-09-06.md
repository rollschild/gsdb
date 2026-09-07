# C++ Style & Best-Practices Review — gsdb

Date: 2026-09-06 · Reviewer: Claude (`cpp-style-review` skill, first run) · Scope: coding style and best-practice conformance only. This is not a bug hunt or a security review; where a guideline violation also happens to be a latent defect, the finding says so in one line and moves on.

Rule citation forms: `CG X.N` = C++ Core Guidelines (exact upstream titles quoted), `EMC++ #n` = Effective Modern C++, `EC++ #n` = Effective C++ 3e, `ESTL #n` = Effective STL, `CCS #n` = Sutter/Alexandrescu C++ Coding Standards, `ToTW #n` = Abseil Tip of the Week, `CERT XXX##-CPP` = SEI CERT C++, `Turner` = cppbestpractices.

---

## 1. Executive summary

| severity | count |
|---|---|
| must | 4 |
| should | 45 |
| consider | 14 |
| **total** | **63** |

Overall: the codebase is in good shape for a from-scratch systems project. It is clang-format clean (0 drift across all files), builds with `-Wall -Wextra -Werror`, uses ownership types consistently (`unique_ptr`, deleted copies, factory functions), has strong address types that most debuggers lack, and already uses C++20/23 features well (concepts, deducing `this`, `<print>`, `<format>`). The findings are mostly about *consistency* and *modern-standard replacements*, plus a handful of ownership/lifetime slips in the ELF layer and the pipe wrapper.

Top items, in suggested order of attention:
1. **#2 (must)** `elf::symbol_name_map_` keys are `std::string_view`s pointing at demangled strings that are `free()`d on the next line — the container owns nothing. Fix with an owning string store (also the only finding here that is a live defect).
2. **#1 (must)** `elf`'s constructor acquires an fd, an mmap, and a `dwarf` without RAII members; any throw after `open()` leaks.
3. **#3 (must)** `pipe` has a destructor but no deleted copy operations — copying one double-closes.
4. **#7 (must)** one stored hit-handler lambda captures `[&]` instead of `[this]`.
5. **#8 (should)** eleven signatures with adjacent `bool hardware, bool internal` parameters and 12 call sites passing bare `true`/`false`.
6. **#16/#17/#28 (should)** the "always initialize" family: indeterminate members in `registers`, `stop_reason`, `line_table::iterator`; constructor bodies that assign instead of initialize; declare-then-assign locals.
7. **#30/#31 (should)** signed/unsigned mixing centered on `virt_addr::operator+(std::int64_t)` and signed-`int` bit manipulation of DR7 masks.
8. **#43 (should)** the hand-rolled `gsdb::span` on a C++23 project; it also made a wrong-length span in a test easy to write.
9. **#49/#50 (should)** every file mixes `<libgsdb/...>` and `"libgsdb/..."` includes; three headers include heavy headers that a forward declaration would replace.
10. **#61/#62 (should)** add `-Wshadow -Wconversion -Wold-style-cast ...` and a `.clang-tidy`; roughly a third of the findings below would then be caught mechanically.

Caveats:
- **HEAD does not compile** (`src/dwarf.cpp`, the in-progress DWARF-expression work); see §2 "Build health". Those lines are not treated as findings.
- `include/libgsdb/dwarf.hpp` gained a `location_list` class during the session, committed as `4915d55` ("executing location lists"); the same commit appended `location_list::eval` at `src/dwarf.cpp:2041-2080`, which landed after the review pass and is **not covered** below. All other line numbers are for that commit (the insertion was at the end of the file, so nothing shifted).
- Core Guidelines snapshot was refreshed today (see §3), so no "offline" caveat applies.

---

## 2. Project profile

- **Standard/toolchain**: C++23 (`CMAKE_CXX_STANDARD 23`, `cxx_std_23` on the library target); GCC 15.2 via Nix flake; CMake ≥ 4.1.2; `LANGUAGES C CXX ASM`.
- **Flags**: `-Wall -Wfatal-errors -Wextra -Werror -g -O1` applied globally for every configuration. No `-Wshadow`, `-Wconversion`, `-Wpedantic`, `-Wold-style-cast`, no sanitizer configuration.
- **Formatting**: `.clang-format` = `BasedOnStyle: Google`, `IndentWidth: 4`. **Zero formatting drift** in all 45 reviewed files (`clang-format --dry-run`). No `.clang-tidy`.
- **House rules that override generic guidance** (from `CLAUDE.md`):
  - Assembly is AT&T syntax everywhere (documentation and code) — not a C++ guideline matter, honored in this report.
  - Factory-pattern types have private constructors with `friend` access; this is why `std::unique_ptr<T>(new T(...))` appears (CG C.150 explicitly allows this case; see #27).
  - X-macro tables (`detail/registers.inc`, `src/include/syscalls.inc`) are a deliberate, documented exception to "no macros" (CG ES.30-33) — only their hygiene is reviewed (#39).
  - `GTest` in the root CMakeLists is already documented as a leftover (#62).
- **Inventory**: 20 headers (`include/libgsdb/*.hpp`, `detail/dwarf.h`, `detail/registers.inc`), 13 library sources, 1 CLI source, 1 test source, 11 test-target sources (+2 `.s`). ≈10.4k lines total, ≈7.4k reviewed C++ (excluding the two constant tables and assembly). Generated/vendored code: none.
- **Build health**: `cmake --build build` fails at `src/dwarf.cpp:1818` (`attr` has no member `as_evaluated_location`). Two more blockers follow once that is resolved: `src/dwarf.cpp:1826` (`stack.push_back(stack.push_back())`) and `src/dwarf.cpp:1851-1855` (`case DW_OP_deref:` declares `auto addr` without a `{}` block, so the following `case` label "crosses initialization"). All three are inside the WIP `dwarf_expression::eval`; they are listed for awareness only.

---

## 3. Knowledge base used

- Effective Modern C++ (all 42 items) — `~/.claude/skills/cpp-style-review/references/effective-modern-cpp.md`.
- C++ Core Guidelines: upstream document dated **Jun 14, 2026**, repository commit `33bcd01` (2026-08-06), fetched **2026-09-06**; 497 rule headings indexed; curated checklist validated against the index (451 IDs, 0 unknown).
- Other sources cited below: CCS, EC++, ESTL, Abseil ToTW, Google C++ Style Guide (the project's clang-format base), SEI CERT C++, Turner's cppbestpractices, LLVM coding standards.

---

## 4. Mechanical pass (leads only; every reported finding was confirmed by reading the code)

clang-tidy 21.1.8 with `cppcoreguidelines-*, modernize-*, readability-*, performance-*, misc-*` (minus the noisiest checks) over all 15 translation units; headers deduplicated across TUs. `src/dwarf.cpp` analysis is partial (compile error above).

| check | unique hits | disposition |
|---|---|---|
| modernize-use-nodiscard | 109 | → #15 (consider) |
| readability-qualified-auto | 96 | style choice (`auto` vs `const auto*`); not reported |
| cppcoreguidelines-pro-bounds-pointer-arithmetic | 76 | inherent to a byte-level parser; concentrated in `cursor`; not reported |
| misc-const-correctness | 61 | folded into #21/#37 |
| cppcoreguidelines-narrowing-conversions | 33 | → #30 |
| performance-enum-size / use-enum-class | 31 / 25 | all in `detail/dwarf.h` constant tables → #40 |
| readability-implicit-bool-conversion | 28 | → #37 |
| cppcoreguidelines-pro-type-vararg | 27 | all `ptrace(...)` calls (variadic C API); not reported |
| cppcoreguidelines-pro-type-reinterpret-cast | 24 | → #32 |
| readability-math-missing-parentheses | 17 | → folded into #31 |
| modernize-use-designated-initializers | 16 | not reported (aggregates are small) |
| cppcoreguidelines-pro-type-member-init / init-variables | 15 / 15 | → #16, #28 |
| cppcoreguidelines-special-member-functions | 13 | → #3 (pipe), others are deleted-copy types (fine) |
| performance-unnecessary-value-param | 12 | → #9 |
| cppcoreguidelines-macro-usage | 11 | X-macro helpers → #39 |
| readability-else-after-return / modernize-return-braced-init-list / modernize-use-ranges | 9 / 9 / 8 | → #36/#37 |
| performance-unnecessary-copy-initialization / for-range-copy | 8 / 2 | → #9 |
| cppcoreguidelines-avoid-do-while | 8 | → #36 |
| pro-type-const-cast / union-access / owning-memory / no-malloc | 5 each | → #22, #25, #4 |
| readability-container-contains / -size-empty | 4 / 4 | → #52, #55 |
| everything else | ≤ 3 each | folded where relevant |

clang-format: 0 files would change. Grep hotspots (confirmed counts): `reinterpret_cast` 24 (bit.hpp 3, elf.cpp 5, process.cpp 5, target.cpp 5, dwarf.cpp 2, tests 2, registers.cpp 1, pipe.cpp 1) · `const_cast` 5 · `dynamic_cast` 3 · `new` 5 (all private-ctor factories) · `free(` 4 · `#define` 15 (guards + X-macros) · `std::endl` 2 · `catch (...)` 1 · `[[maybe_unused]]` 19 · `and/or/not` 257 vs `&&/||/!` 77 · `std::cerr` 17 / `std::cout` 4 / `std::print` 26 (CLI).

---

## 5. Findings

Format: `Rules` → `Where` → `What` → `Why` → `Fix`. Repeated patterns are clustered into one finding.

### 5.1 Ownership & lifetime

### 1. `elf` constructor acquires three resources without RAII members — must
Rules: CG E.6 "Use RAII to prevent leaks"; CG C.31 "All resources acquired by a class must be released by the class's destructor"; CG R.1 "Manage resources automatically using resource handles and RAII"; CG C.49 "Prefer initialization to assignment in constructors"; CCS #13; EC++ #13.
Where: `src/elf.cpp:23-61`, `include/libgsdb/elf.hpp:91-98`.
What:
```cpp
if ((fd_ = open(path.c_str(), O_RDONLY)) < 0) error::send_errno(...);
struct stat stats;
if (fstat(fd_, &stats) < 0) error::send_errno(...);          // fd_ leaks
...
if ((ret = mmap(0, file_size_, PROT_READ, MAP_SHARED, fd_, 0)) == MAP_FAILED) { close(fd_); ... }
...
dwarf_ = std::make_unique<dwarf>(*this);                       // any throw here leaks fd_ and the mapping
```
Why: a constructor that throws never runs its destructor, so every resource acquired before the throw must already be owned by a member that cleans itself up. Here only the `mmap` failure path cleans up, and only for the fd. `parse_*`/`dwarf` construction throw `gsdb::error` on malformed input, which is exactly when the leak happens.
Fix: introduce two tiny RAII members and initialize them in the mem-initializer list:
```cpp
class unique_fd { int fd_ = -1; public: explicit unique_fd(int fd); ~unique_fd(); /* move-only */ int get() const; };
class mapped_file { const std::byte* data_ = nullptr; std::size_t size_ = 0; /* munmap in dtor */ };
elf::elf(const std::filesystem::path& path)
    : path_(path), fd_(open_or_throw(path)), map_(map_or_throw(fd_.get())), header_(read_header(map_)) { ... }
```
This also resolves the indeterminate `fd_`/`file_size_`/`data_`/`header_` members (#16) and the `path_ = path;` assignment (#17).

### 2. `symbol_name_map_` keys view memory that is freed immediately — must
Rules: CG SL.str.1 "Use `std::string` to own character sequences"; CG SL.str.2 "Use `std::string_view` or `gsl::span<char>` to refer to character sequences"; CG R.12 "Immediately give the result of an explicit resource allocation to a manager object"; CG R.10 "Avoid `malloc()` and `free()`"; Pro.lifetime; CERT MEM50-CPP.
Where: `include/libgsdb/elf.hpp:111`; `src/elf.cpp:178-188`.
What:
```cpp
std::unordered_multimap<std::string_view, Elf64_Sym*> symbol_name_map_;   // elf.hpp:111
auto demangled_name = abi::__cxa_demangle(mangled_name.data(), nullptr, nullptr, &demangle_status);
if (demangle_status == 0) {
    symbol_name_map_.insert({demangled_name, &symbol});   // string_view key → malloc'd buffer
    free(demangled_name);                                 // ...which is released here
}
```
Why: `string_view` is a non-owning reference type; the guideline is that the *owner* must outlive every view. The mangled-name keys are fine (they view the mmap, whose lifetime is documented in CLAUDE.md), but the demangled keys dangle the moment `free()` runs. This is the one finding in this report that is also a live defect: `get_symbols_by_name("main")`-style lookups by demangled name hash and compare freed memory.
Fix: own the demangled strings and view them:
```cpp
std::vector<std::string> demangled_names_;      // stable storage: reserve(symbol_table_.size()) once, or use std::deque
std::unique_ptr<char, decltype(&std::free)> demangled{abi::__cxa_demangle(...), &std::free};   // R.12
if (demangle_status == 0) {
    auto& owned = demangled_names_.emplace_back(demangled.get());
    symbol_name_map_.emplace(owned, &symbol);
}
```
(`std::vector<std::string>` is fine as storage only if it is never resized after the views are taken; `std::deque<std::string>` or a `reserve()` up front removes that trap. Alternatively key the multimap by `std::string` and enable C++20 heterogeneous lookup with `std::hash<std::string_view>`-compatible hasher + `std::equal_to<>`.)

### 3. `pipe` is implicitly copyable and would double-close — must
Rules: CG C.21 "If you define or `=delete` any copy, move, or destructor function, define or `=delete` them all"; EMC++ #17; CCS #52-#53; EC++ #6.
Where: `include/libgsdb/pipe.hpp:9-33`.
What: the class declares `~pipe()` and nothing else among the special members; `pipe(const pipe&)` and `operator=` are therefore generated and copy `fds_[2]` bit-for-bit.
Why: a resource-owning type with a user-declared destructor must decide copy/move explicitly; the implicit copy here means two destructors call `close()` on the same descriptors. The other resource types in the project (`process`, `elf`, `breakpoint_site`, `watchpoint`) do this correctly — `pipe` is the odd one out.
Fix:
```cpp
pipe(const pipe&) = delete;
pipe& operator=(const pipe&) = delete;
pipe(pipe&& other) noexcept : fds_{std::exchange(other.fds_, {-1, -1})} {}   // optional
pipe& operator=(pipe&&) noexcept;                                            // optional
std::array<int, 2> fds_{-1, -1};                                             // also #16 / ES.27
```

### 4. Manual C resource pairs without RAII — should
Rules: CG R.1; CG R.10 "Avoid `malloc()` and `free()`"; CG R.12; CG E.19 "Use a `final_action` object to express cleanup if no suitable resource handle is available"; CG CPL.3 "If you must use C for interfaces, use C++ in the calling code using such interfaces"; CERT FIO51-CPP "Close files when they are no longer needed".
Where (6 patterns, 14 sites):
- `readline()` result + two `free(line)` paths — `tools/gsdb.cpp:971-986`.
- `popen`/`pclose` and `getline(&line, &len, pipe)`/`free(line)` with early-return bookkeeping — `test/tests.cpp:61-90`.
- `open("/dev/null")` … `close(dev_null)` at the end of 6 test cases — `test/tests.cpp:578/604, 773/814, 818/865, 888/904, 908/933` (leaks on any failed `REQUIRE`, which throws).
- `mkdtemp(tmp_dir)` return value unchecked and the directory (`/tmp/gsdb-XXXXXX/linux-vdso.so.1`) never removed — `src/target.cpp:49-52`.
- `abi::__cxa_demangle` … `free` — `src/elf.cpp:182-186` (see #2).
- `std::ifstream file{path.string()}` opened without checking `is_open()` — `tools/gsdb.cpp:747` (SL.io.2, minor).
Why: every one of these is a resource whose release depends on control flow reaching a specific line; the project already has the right vocabulary (`gsdb::pipe` is a proper RAII wrapper) but does not apply it to fds, C strings, and temp files.
Fix: a small `include/libgsdb/detail/raii.hpp` with `unique_fd` (reused by #1), `using c_string_ptr = std::unique_ptr<char, decltype(&std::free)>`, `unique_pipe_stream` for `popen`, and a `scope_exit`/`final_action` (`std::experimental::scope_exit` or ten lines). In tests, an RAII `dev_null` fixture removes 12 lines of manual bookkeeping.

### 5. `line_table::entry::file_entry` points into a `mutable` vector that can grow — should
Rules: CG R.3 "A raw pointer (a `T*`) is non-owning"; CG ES.65 "Don't dereference an invalid pointer"; Pro.lifetime; EMC++ #16 (mutable in const member functions).
Where: `include/libgsdb/dwarf.hpp:343` (`mutable std::vector<file> file_names_`), `:362` (`file* file_entry`), `src/dwarf.cpp:1380` (pointer taken), `:1465` (`file_names_.push_back(file)` from `DW_LNE_define_file`).
What: each yielded row stores `&table_->file_names_[idx - 1]`; a later `DW_LNE_define_file` opcode `push_back`s into the same vector and can reallocate, invalidating every `file_entry` pointer already handed to callers (`source_location::file`, `stack_frame::location`).
Why: raw pointers into a growable container are only safe while the container is not modified; the guideline is to either make that invariant true (stable storage) or store an index/handle instead. GCC does not emit `DW_LNE_define_file` today, which is why this has not bitten; it is still an unstated lifetime contract.
Fix: store the index (`std::size_t file_index` already exists — expose a `const file& file() const` accessor on the table instead of a cached pointer), or make `file_names_` a `std::deque<file>` (stable element addresses on `push_back`) and document the invariant. Also document that `line_table`, `dwarf` (`function_index_`), and `call_frame_information` (`cie_map_`) are single-threaded caches (EMC++ #16).

### 6. `std::optional<T*>` returns and undocumented non-owning pointers — consider
Rules: CG F.60 "Prefer `T*` over `T&` when 'no argument' is a valid option"; ToTW #163; CG C.32 "If a class has a raw pointer (`T*`) or reference (`T&`), consider whether it might be owning".
Where: `include/libgsdb/elf.hpp:37, 71-77` (five `std::optional<const Elf64_Shdr*>`/`std::optional<const Elf64_Sym*>` returns); `src/process.cpp:601` (`std::optional<breakpoint_site*> to_reenable`); `include/libgsdb/target.hpp:31` (`thread_state* state` relies on `unordered_map` node stability); `include/libgsdb/breakpoint_site.hpp:45,47`, `watchpoint.hpp:47`, `stack.hpp:73` (back-pointers).
Why: a pointer already has an "absent" state; wrapping it in `optional` doubles the checks callers must write (`sym.value()->st_name`) without adding information. Non-owning back-pointers are fine (the project is consistent about ownership living in `unique_ptr`s) but their validity assumptions (e.g. `std::unordered_map` never invalidates element pointers on rehash) deserve a one-line comment where they are stored.
Fix: return `const Elf64_Sym*` (nullptr = absent) or, where callers always dereference, a reference plus a `bool has_*` query; replace `std::optional<breakpoint_site*>` with `breakpoint_site* to_reenable = nullptr;`.

### 5.2 Interfaces & functions

### 7. Stored callback captures by reference — must
Rules: CG F.53 "Avoid capturing by reference in lambdas that will be used non-locally, including returned, stored on the heap, or passed to another thread"; CG F.54 "When writing a lambda that captures `this` or any class data member, don't use `[=]` default capture" (same reasoning); EMC++ #31.
Where: `src/target.cpp:402-405`.
What:
```cpp
debug_state_bp.install_hit_handler([&] {
    reload_dynamic_libraries();
    return true;
});
```
Why: the closure is stored in a `breakpoint` owned by `target::breakpoints_` and runs much later; `[&]` captures `this` by reference implicitly and hides that fact from the reader. The other stored handler in the same file (`target.cpp:79`, `[target = tgt.get()]`) already does this right.
Fix: `install_hit_handler([this] { reload_dynamic_libraries(); return true; });`

### 8. Adjacent boolean parameters and bare `true`/`false` at call sites — should
Rules: CG I.24 "Avoid adjacent parameters that can be invoked by the same arguments in either order"; CG I.4 "Make interfaces precisely and strongly typed"; ToTW #94 (callsite readability and `bool` parameters); Enum.2 "Use enumerations to represent sets of related named constants".
Where (11 signatures): `include/libgsdb/breakpoint.hpp:89, 110, 129-130, 148`; `include/libgsdb/breakpoint_site.hpp:38-42`; `include/libgsdb/target.hpp:103-111`; `include/libgsdb/process.hpp:117-119` (`bool debug`), `:161-169`; `include/libgsdb/process.hpp:250` (`bool terminate_on_end, bool is_attached`). 12 call sites pass literals: `src/target.cpp:69, 77, 193, 401`; `test/tests.cpp:146, 166, 186, 408, 448, 487, 498, 551`.
What: `create_breakpoint_site(address, false, true)`, `process::launch(path, true, channel.get_write())`, `new process(pid, /*terminate_on_end=*/true, debug)`.
Why: two adjacent `bool`s swap silently; a reader of `create_address_breakpoint(entry_point, false, true)` has to open the header. The tests already show the better habit once (`bool close_on_exec = false; gsdb::pipe channel(close_on_exec);`).
Fix: `enum class stoppoint_kind { software, hardware };`, `enum class visibility { user, internal };`, `enum class launch_mode { traced, untraced };` (or one `struct breakpoint_options { stoppoint_kind kind = software; visibility vis = user; }` used with designated initializers). Where the API must stay, at least name the arguments at call sites (`/*hardware=*/false, /*internal=*/true`) as `process.cpp:490` already does.

### 9. Parameter passing: read-only `std::string`/`path`/`vector` by value, sinks not moved — should
Rules: CG F.16 "For 'in' parameters, pass cheaply-copied types by value and others by reference to `const`"; CG F.18 "For 'will-move-from' parameters, pass by `X&&` and `std::move` the parameter"; EMC++ #41; ToTW #77, #117.
Where:
- read-only by value: `dwarf::find_functions(std::string)` (`dwarf.hpp:442`, `dwarf.cpp:1287`), `target::find_functions(std::string)` (`target.hpp:101`), `elf_collection::get_elf_by_path(std::filesystem::path)` (`elf.hpp:144`), `line_table::get_entries_by_line(std::filesystem::path, ...)` (`dwarf.hpp:330`), `target::get_line_entries_by_line(std::filesystem::path, ...)` (`target.hpp:127`), `parse_line_table_file(cursor&, std::filesystem::path compilation_dir, ...)` (`dwarf.cpp:514`), `process::launch(std::filesystem::path)` (`process.hpp:118`), `target::launch(std::filesystem::path)` (`target.hpp:50`), `get_section_load_bias`/`get_entry_point_offset(std::filesystem::path)` (`tests.cpp:53, 95`);
- `const std::vector<gsdb::die> inline_stack` by *const value* (`stack.hpp:66-71`, `stack.cpp:112-114, 134-136`) — the `const` makes the copy impossible to move from;
- sink parameters copied instead of moved: `target::create_function_breakpoint` / `create_line_breakpoint` take `std::string function_name` / `std::filesystem::path file` by value and then copy them into the `new function_breakpoint(*this, function_name, ...)` (`target.cpp:322-331`);
- copies of `const std::string&` elements into locals used read-only: `auto command = args[1];` ×4 (`gsdb.cpp:520, 564, 653, 919`), `auto mode_text = args[3];` (`:620`), `auto path = data[0];` (`:501`); `auto regs = target.get_stack().regs();` copies a ~1 KiB `registers` to print it (`gsdb.cpp:351`); `for (auto die : ...)` (`breakpoint.cpp:44`), `for (auto [reg, rule] : ctx.register_rules)` (`dwarf.cpp:833`), `for (auto child : current.children())` (`dwarf.cpp:1350`).
Why: `std::filesystem::path` and `std::string` are not "cheaply copied" (heap allocation); by-value is only right for sinks, and sinks must then be moved. The rest are needless allocations on hot paths (`find_functions` runs for every breakpoint resolve, `get_entries_by_line` for every CU).
Fix: `const std::string&`/`std::string_view` for lookups (`unordered_multimap<std::string,...>::equal_range` needs a `std::string` unless heterogeneous lookup is enabled — see #55), `const std::filesystem::path&` for paths, `const std::vector<die>&` for the inline stacks, `std::move(function_name)`/`std::move(file)` in the factories, `const auto&` for the locals and loop variables.

### 10. `std::unique_ptr<target>&` parameters where `target&` is meant — should
Rules: CG R.30 "Take smart pointers as parameters only to explicitly express lifetime semantics"; CG F.7 "For general use, take `T*` or `T&` arguments rather than smart pointers"; ToTW #188.
Where: `tools/gsdb.cpp:916-917` (`handle_command(std::unique_ptr<gsdb::target>& target, ...)`), `:969` (`main_loop(std::unique_ptr<gsdb::target>&)`).
Why: neither function reseats or takes ownership of the pointer; a `unique_ptr&` parameter tells the reader "I may replace the owned object" (R.33), which is false here, and it forces every call site to have a `unique_ptr` at hand.
Fix: `void handle_command(gsdb::target& target, std::string_view line)`; `void main_loop(gsdb::target& target)`; call with `*target`.

### 11. Single-argument constructors that are not `explicit` — should
Rules: CG C.46 "By default, declare single-argument constructors explicit"; ToTW #142; Google:Implicit Conversions; HIC++ 7.1.4.
Where: `include/libgsdb/elf.hpp:23` (`elf(const std::filesystem::path&)`), `include/libgsdb/disassembler.hpp:19` (`disassembler(process&)`), `include/libgsdb/dwarf.hpp:379` (`line_table::iterator(const line_table*)`), `:425` (`dwarf(const elf&)`), `:554` (`children_range(die)`).
Why: each of these lets a `path`, a `process&`, an `elf&`, or a `die` silently convert into a heavyweight object (e.g. a function taking `const elf&` accepts a string literal and mmaps a file). `cursor(span)` and `die(const std::byte*)` in the same layer are already `explicit`. `span(const std::vector<U>&)` (`types.hpp:67`) is intentionally implicit, mirroring `std::span`, and is fine.
Fix: add `explicit` to the five constructors; existing code uses direct initialization everywhere so nothing else changes.

### 12. Unused parameters left named, stale `[[maybe_unused]]`, dead locals — should
Rules: CG F.9 "Unused parameters should be unnamed"; CG I.4 (a parameter the function ignores is a false promise); CG ES.? / NL.1 (dead code).
Where:
- ignored parameters kept in the API: `process::set_hardware_breakpoint([[maybe_unused]] id, addr)` (`process.cpp:717-718`), `process::set_watchpoint([[maybe_unused]] id, ...)` (`:796`) — callers pass an id the function never uses;
- unused but named: `parse_eh_frame_pointer([[maybe_unused]] const elf& elf, ...)` (`dwarf.cpp:232`), `parse_compile_unit(..., [[maybe_unused]] const elf& obj, ...)` (`:438`), `execute_cfi_instruction(..., [[maybe_unused]] file_addr pc)` (`:675`), `read_frame_base_result(..., [[maybe_unused]] const registers& regs)` (`:865`), `create_inline_stack_frames(..., [[maybe_unused]] file_addr pc)` (`stack.cpp:134-136`), `handle_watchpoint_list(..., [[maybe_unused]] const std::vector<std::string>& args)` (`gsdb.cpp:584-585`);
- `[[maybe_unused]]` on functions that *are* used: `get_next_id` (`breakpoint_site.cpp:12`), `is_prefix` (`gsdb.cpp:58`), `parse_fde` (`dwarf.cpp:330`);
- dead locals: `eh_hdr_start`, `text_section_start`, `version` (`dwarf.cpp:366-376`), `return_address_register` (`:295`), `len` (`:1445`), `[[maybe_unused]] auto pid = process_->pid();` (`target.hpp:144`), `elf_name` (`target.cpp:354`, computed and never read — no warning because `std::string` has a non-trivial destructor).
Why: `[[maybe_unused]]` is a promise to the compiler that hides real dead code from `-Wunused`; the guideline's answer is to leave the parameter unnamed (`file_addr /*pc*/`) or drop it from the signature.
Fix: remove `id` from `set_hardware_breakpoint`/`set_watchpoint`; unname the six parameters; delete the dead locals and stale attributes (the `version` read must stay as `(void)cur.u8();`/`std::ignore = cur.u8();` because it advances the cursor).

### 13. `stack::up()`/`down()` state no preconditions — should
Rules: CG I.5 "State preconditions (if any)"; CG I.6 "Prefer `Expects()` for expressing preconditions"; CG ES.104 "Don't underflow".
Where: `include/libgsdb/stack.hpp:47-48`; called from user input at `tools/gsdb.cpp:956, 959`.
What: `void up() { ++current_frame_; }  void down() { --current_frame_; }` on a `std::size_t`, with `current_frame()` indexing `frames_[current_frame_]` unchecked.
Why: these are driven directly by the `up`/`down` REPL commands, so the precondition (`current_frame_ + 1 < frames_.size()`, `current_frame_ > inline_height_`) is violated by ordinary user input; asserts would not help in release builds, and there is no comment stating the contract.
Fix: check and throw `gsdb::error` (the CLI already prints those), or return `bool`; document the contract in the header either way.

### 14. Small API-shape issues — should
Rules: CG C.9 "Minimize exposure of members" (inverse: a type used in the public interface must be nameable); CG F.49 "Don't return `const T`"; CG NL.25 "Don't use `void` as an argument type".
Where:
- `disassembler::instruction` is declared in the private section but returned from the public `disassemble()` (`include/libgsdb/disassembler.hpp:13-16, 24`): callers can only hold it via `auto`/`decltype`. → make the struct public.
- `const std::filesystem::path file() const` (`include/libgsdb/breakpoint.hpp:123`) returns a `const` prvalue, which disables move-out. → `const std::filesystem::path&` (or `std::filesystem::path`).
- `std::function<bool(void)>` (`include/libgsdb/breakpoint.hpp:72, 99`) → `std::function<bool()>`.
- `pipe::write(std::byte* from, std::size_t bytes)` (`include/libgsdb/pipe.hpp:27`) takes a non-const pointer + length pair → `span<const std::byte>` (CG I.13 "Do not pass an array as a single pointer").
Fix: move `struct instruction` above `public:` in `disassembler`; change `file()` to return `const std::filesystem::path&`; `std::function<bool()>`; `void write(span<const std::byte> data);` with `::write(fds_[write_fd], data.begin(), data.size())`.

### 15. `[[nodiscard]]` on factories and pure queries — consider
Rules: Turner ("`[[nodiscard]]`"); `modernize-use-nodiscard` (109 hits); spirit of CG F.20.
Where: factories `process::launch/attach` (`process.hpp:117-120`), `target::launch/attach` (`target.hpp:49-52`), `create_*` functions returning references that must be used to `enable()`, parsers `to_integral`/`to_float`/`parse_vector` (`parse.hpp:16, 52, 65, 93`), `disassemble`, `read_memory`, `find_functions`, `syscall_name_to_id`.
Why: a discarded `process::launch(...)` result kills the child immediately (destructor semantics) — exactly the kind of silent mistake the attribute exists for. Do **not** apply it blanket-wise: `wait_on_signal()`'s result is legitimately ignored in many tests.
Fix: add `[[nodiscard]]` to factories, converters, and non-mutating queries; skip functions with useful side effects.

### 5.3 Classes

### 16. Members and objects that can be read before they are written — should
Rules: CG ES.20 "Always initialize an object"; CG C.41 "A constructor should create a fully initialized object"; CG C.48 "Prefer default member initializers to member initializers in constructors for constant initializers"; CCS #19; EC++ #4.
Where:
- `registers`: `registers() = default;` is public while `user data_; process* proc_; pid_t tid_;` have no initializers (`include/libgsdb/registers.hpp:19, 53-54, 62-63`) — a default-constructed `registers` (used as `stack_frame::regs` element type and via copies in `unwind`) has an indeterminate `proc_`;
- `stop_reason() = default;` with `process_state reason; std::uint8_t info; pid_t tid;` uninitialized (`include/libgsdb/process.hpp:52, 75-79`); `thread_state::tid` (`:83`) is the only member without an initializer;
- `line_table::iterator`: `const line_table* table_; const std::byte* pos_;` without initializers (`include/libgsdb/dwarf.hpp:394-397`) — `end()` returns `{}` and works only because a defaulted-on-first-declaration constructor makes `{}` zero-initialize; `line_table::iterator it;` would not;
- `line_table::entry::is_stmt` has no initializer while its 9 neighbours do (`dwarf.hpp:354`);
- `breakpoint_site` (`breakpoint_site.hpp:46-54`) and `watchpoint` (`watchpoint.hpp:46-51`) initialize `parent_`/`hardware_register_index_`/`data_` inline but `id_`, `is_enabled_`, `saved_data_`, `is_hardware_`, `is_internal_`, `mode_`, `size_` in every constructor instead;
- `elf`: `int fd_; std::size_t file_size_; std::byte* data_; Elf64_Ehdr header_;` (`elf.hpp:91-95`) — see #1;
- `pipe::fds_` (`pipe.hpp:32`) — see #3;
- `from_bytes<To>`: `To ret;` (`bit.hpp:22`) is fine only for trivially copyable `To` — see #41.
Why: default member initializers make the "empty" state a real, documented state instead of an accident of value-initialization, and they remove the repeated `is_enabled_{false}, saved_data_{}` lists from constructors.
Fix: `user data_{}; process* proc_ = nullptr; pid_t tid_ = 0;` (registers); `process_state reason = process_state::stopped; std::uint8_t info = 0; pid_t tid = 0;` (stop_reason); `const line_table* table_ = nullptr; const std::byte* pos_ = nullptr;`; `bool is_stmt = false;`; `bool is_enabled_ = false; std::byte saved_data_{}; bool is_hardware_ = false; bool is_internal_ = false;`.

### 17. Constructors that assign in the body instead of initializing — should
Rules: CG C.49 "Prefer initialization to assignment in constructors"; CG C.45 "Don't define a default constructor that only initializes data members; use default member initializers instead"; CCS #48; EC++ #4.
Where: `src/breakpoint_site.cpp:28` (`id_ = is_internal_ ? -1 : get_next_id();`), `src/watchpoint.cpp:28` (`id_ = get_next_id();`), `src/breakpoint.cpp:18`, `src/elf.cpp:24-26` (`path_ = path; if ((fd_ = open(...)))`), `src/dwarf.cpp:879-881` (`compile_units_ = ...; cfi_ = ...;`), `src/dwarf.cpp:895` (`line_table_ = parse_line_table(*this);`).
Why: assignment first default-constructs the member and then overwrites it; the pattern also prevents members from being `const` and hides the initialization order from readers. (The two `dwarf.cpp` cases pass `*this` to a parser before construction completes — legal because only already-initialized members are read, but worth one comment.)
Fix: `: id_(is_internal ? -1 : get_next_id())`, `: path_(path)`, `: compile_units_(parse_compile_units(*this, parent)), cfi_(parse_call_frame_information(*this))` (with a comment on the partially-constructed `*this`), and so on.

### 18. `breakpoint` exposes protected data and duplicates the site-creation dance in every subclass — should
Rules: CG C.133 "Avoid `protected` data"; CG ES.3 "Don't repeat yourself, avoid redundant code"; CG F.1 "'Package' meaningful operations as carefully named functions"; CCS #41.
Where: `include/libgsdb/breakpoint.hpp:83-99` (protected `id_`, `target_`, `is_enabled_`, `is_hardware_`, `is_internal_`, `breakpoint_sites_`, `next_site_id_`, `on_hit_`); the same 6-line block appears four times: `src/breakpoint.cpp:31-38, 65-71, 78-84, 117-123`.
What:
```cpp
if (!breakpoint_sites_.contains_address(load_address)) {
    auto& new_site = target_->get_process().create_breakpoint_site(this, next_site_id_++, load_address, is_hardware_, is_internal_);
    breakpoint_sites_.push(&new_site);
    if (is_enabled_) new_site.enable();
}
```
Why: protected data couples every subclass to the base's representation (renaming `next_site_id_` touches four files); the duplicated block is exactly the operation that should be the base class's protected *function*.
Fix: make the data private and add `protected: breakpoint_site& add_site(virt_addr address);` that does the lookup/create/push/enable; `resolve()` implementations then become a loop over addresses.

### 19. Virtual `resolve()` is called from constructors — should
Rules: CG C.82 "Don't call virtual functions in constructors and destructors"; CG C.50 "Use a factory function if you need 'virtual behavior' during initialization"; EC++ #9; CERT OOP50-CPP.
Where: `include/libgsdb/breakpoint.hpp:113-115, 133-135, 149-151`.
Why: it works today only because each call sits in the most-derived class's constructor body; a future subclass of `function_breakpoint`, or moving the call into `breakpoint`'s constructor, changes which override runs with no compiler diagnostic. The factories already exist (`target::create_*_breakpoint`), so this is the textbook C.50 case.
Fix: remove the `resolve()` calls from the constructors and call `bp.resolve()` in `target::create_address_breakpoint`/`create_function_breakpoint`/`create_line_breakpoint` after `push()`; alternatively mark the three concrete classes `final` and keep a comment — the factory route is cleaner.

### 20. `dynamic_cast` chain to describe breakpoint kinds — should
Rules: CG C.146 "Use `dynamic_cast` where class hierarchy navigation is unavoidable"; CG C.153 "Prefer virtual function to casting"; CCS #90 "Avoid type switching; prefer polymorphism".
Where: `tools/gsdb.cpp:453-463`.
What: `if (auto func_bp = dynamic_cast<gsdb::function_breakpoint*>(&bp)) ... else if (dynamic_cast<gsdb::line_breakpoint*>) ... else if (dynamic_cast<gsdb::address_breakpoint*>)`.
Why: adding a fourth breakpoint kind silently prints nothing here; the hierarchy already has one virtual (`resolve()`), so a second one costs nothing.
Fix: `virtual std::string describe() const = 0;` on `breakpoint` (`"function = main"`, `"file = x.cpp, line = 17"`, `"address = 0x..."`), and the CLI prints `bp.describe()`.

### 21. Const-correctness holes — should
Rules: CG Con.2 "By default, make member functions `const`"; CG Con.4 "Use `const` to define objects with values that do not change after construction"; CG C.9 "Minimize exposure of members"; EC++ #3, #28.
Where:
- `stoppoint_collection::get_in_region(...) const` returns `std::vector<Stoppoint*>` — mutable access out of a const object (`include/libgsdb/stoppoint_collection.hpp:65, 219-229`);
- `compile_unit` stores `dwarf* parent_` (non-const) so that `abbrev_table() const` can call the non-const `dwarf::get_abbrev_table()` and mutate `abbrev_tables_` (`include/libgsdb/dwarf.hpp:410, 417, 428-429, 469-470`; `src/dwarf.cpp:884-901`). The same file already solves the identical problem correctly with `mutable function_index_` + `index() const`;
- `elf::data_` is `std::byte*` for a `PROT_READ` mapping (`include/libgsdb/elf.hpp:94`), which is why `get_section_name`/`get_string` need `reinterpret_cast<char*>` (`src/elf.cpp:87, 121`) instead of `const char*`, and why `section_map_`/`symbol_*_map_` hold non-const `Elf64_Shdr*`/`Elf64_Sym*` into read-only memory;
- `process::write_memory` is non-const while `read_memory` is const (`process.hpp:185-189`) — defensible ("mutates the inferior"); state the rule in a comment so it does not look accidental.
Fix: `std::vector<const Stoppoint*>` from the const overload (add a non-const overload if needed); `mutable abbrev_tables_` + `get_abbrev_table(...) const` + `const dwarf* parent_`; `const std::byte* data_` and `const Elf64_Shdr*`/`const Elf64_Sym*` in the maps.

### 22. `const_cast` const-overload idiom where the project already uses deducing `this` — should
Rules: CG ES.50 "Don't cast away `const`"; CG ES.3 "Don't repeat yourself, avoid redundant code"; EMC++ (Meyers' own idiom, superseded in C++23).
Where: `include/libgsdb/stoppoint_collection.hpp:109, 126, 162, 177`; `src/process.cpp:151`. Precedent for the modern form: `include/libgsdb/target.hpp:77-82` (`auto& get_stack(this auto& self, ...)`) — and the commented-out old version at `target.hpp:67-75` shows the migration was already considered.
Why: the `const_cast<T*>(this)->f()` trick is tolerated by the guidelines, but on a C++23 project that already uses explicit object parameters it is an inconsistency, and each site is five lines that can be one.
Fix:
```cpp
auto& get_by_id(this auto& self, typename Stoppoint::id_type id) {
    auto it = self.find_by_id(id);
    if (it == std::end(self.stoppoints_)) error::send("Invalid stoppoint ID!");
    return **it;
}
```
(same for `get_by_address`, `find_by_*`, and `process::get_registers`). Then delete the commented-out Meyers overload in `target.hpp`.

### 23. Comparison operators: incomplete sets and C++20 defaults — should
Rules: CG C.86 "Make `==` symmetric with respect to operand types and `noexcept`"; CG C.161 "Use non-member functions for symmetric operators"; C++20 `operator<=>` (EMC++ addenda); `modernize-use-...`.
Where: `include/libgsdb/types.hpp:40-50` (`virt_addr` has `== != < > >=` but **no `<=`**), `:105-126` (`file_addr` hand-writes all six, four of them with `assert`), `include/libgsdb/dwarf.hpp:144-145, 387-388, 600-601` (`==`/`!=` pairs on three iterators; `range_list::iterator::operator==` takes its argument by value — a ~70-byte copy per comparison — while `line_table::iterator` takes `const&`), `:364-368` (`line_table::entry::operator==`).
Why: asymmetric, incomplete operator sets are exactly what `<=>` removes; since C++20, `!=` is synthesized from `==` and the five ordering operators from `<=>`.
Fix: `virt_addr`: `friend bool operator==(virt_addr, virt_addr) = default; friend auto operator<=>(virt_addr, virt_addr) = default;`. `file_addr`: keep the same-ELF assertion inside a hand-written `friend std::strong_ordering operator<=>(const file_addr&, const file_addr&)` plus a defaulted `==`. Iterators/`entry`: keep only `==` (drop `!=`), take `const iterator&`.

### 24. `eh_hdr::parent` is written but never read; `const` data member — should
Rules: CG C.12 "Don't make data members `const` or references in a copyable or movable type"; NL.1 (dead code).
Where: `include/libgsdb/dwarf.hpp:46-60` (`const std::size_t count;` and `call_frame_information* parent;`), `:72` (`eh_hdr_.parent = this;`) — no other use of `.parent`/`->parent` in the tree.
Why: the back-pointer is dead weight that also makes `call_frame_information` fragile (it would dangle if the object were ever made movable); the `const` member makes `eh_hdr` non-assignable for no benefit.
Fix: delete `parent`; drop `const` from `count` (or keep `eh_hdr` a pure aggregate).

### 25. Anonymous union with an external discriminator — consider
Rules: CG C.181 "Avoid 'naked' `union`s"; CG C.182 "Use anonymous `union`s to implement tagged unions"; CG P.4.
Where: `include/libgsdb/process.hpp:42-49` (`bool entry;` + `union { std::array<std::uint64_t, 6> args; std::int64_t ret; };`), accessed at `src/process.cpp:833, 845` and `tools/gsdb.cpp:272-276`.
Why: nothing stops reading `ret` on an entry stop; `std::variant<std::array<std::uint64_t, 6>, std::int64_t>` (or two `std::optional`s) makes the discriminant intrinsic and the CLI code a `std::visit`.
Fix: `std::variant<syscall_entry, syscall_exit>` with two tiny structs, or keep the union but wrap it behind `args()`/`ret()` accessors that assert on `entry`.

### 26. `process` carries several responsibilities and exposes implementation steps publicly — consider
Rules: CG F.2 "A function should perform a single logical operation" (class-level analogue: CCS #5 "Give one entity one cohesive responsibility"); CG C.9 "Minimize exposure of members".
Where: `include/libgsdb/process.hpp:115-301` — process control, per-thread state, memory access, register I/O, hardware debug-register slot allocation (`set_hardware_stoppoint` and the DR7 encoders in `process.cpp:61-113`), syscall catch policy, auxv parsing; and `handle_signal`, `cleanup_exited_threads`, `report_thread_lifecycle_event`, `stop_running_threads` are public (`:232-240`) although only `process` itself calls them.
Why: not wrong, but the class is the coupling point for the whole library (every header ends up including `process.hpp`, see #50). Moving the DR7 slot bookkeeping into a small `hardware_debug_registers` helper and the auxv reader into a free function would shrink the surface, and the four internal steps can be private.
Fix: private the four internals now (cheap); consider the extraction when the multi-thread work settles.

### 27. `std::unique_ptr<T>(new T(...))` for private-constructor factories — consider (note only)
Rules: CG C.150 "Use `make_unique()` to construct objects owned by `unique_ptr`s"; EMC++ #21; ToTW #134 (the private-constructor exception).
Where: `src/target.cpp:318-331`; `src/process.cpp:582-584, 593-595, 808-810`.
Why: `make_unique` cannot reach a private constructor; the guidelines accept `new` inside the befriended factory. The only cost is that a throw between `new` and the `unique_ptr` construction would leak — impossible here since the `unique_ptr` is constructed in the same full-expression.
Fix: none required. If you want `make_unique` everywhere, the usual trick is a private `struct private_tag {}` constructor parameter that only the factory can name.

### 5.4 Expressions & statements

### 28. Declare-then-assign locals and assignment inside conditions — should
Rules: CG ES.20 "Always initialize an object"; CG ES.22 "Don't declare a variable until you have a value to initialize it with"; CG ES.10 "Declare one name (only) per declaration"; CCS #18-#19.
Where:
- assignment inside the condition: `src/elf.cpp:26` (`if ((fd_ = open(...)) < 0)`), `:42` (`if ((ret = mmap(...)) == MAP_FAILED)`), `src/pipe.cpp:45-49` (`int chars_read; if ((chars_read = ::read(...)) < 0)`), `src/process.cpp:382-386` (`pid_t tid; if ((tid = waitpid(...)) < 0)`), `:433-434` (`pid_t pid; if ((pid = fork()) < 0)`);
- declared without a value: `int status;` (`process.cpp:118`), `int wait_status;` (`:177, :320, :379`), `std::uint64_t word;` (`:680`), `std::uint64_t id, value;` (`:924`, also two names in one declaration), `struct stat stats; void* ret;` (`elf.cpp:30, 36`), `std::size_t size;` / `std::size_t offset;` / `std::uint64_t idx;` (`dwarf.cpp:1043, 1067, 1532`), `gsdb::stoppoint_mode mode; mode = ...` (`gsdb.cpp:630-633`);
- `siginfo_t info;` (`process.cpp:815`) is an out-parameter for `ptrace` — fine, but `siginfo_t info{};` costs nothing.
Why: `pid_t pid = fork(); if (pid < 0)` reads in one pass and lets the variable be `const`; the `switch`-then-assign cases (`size`, `offset`, `idx`) are the ES.28 pattern (initialize via a lambda or a small helper returning the value).
Fix: as above; `const auto [id, value] = read_pair(auxv);` or two declarations for `get_auxv`.

### 29. Magic constants — should
Rules: CG ES.45 "Avoid 'magic constants'; use symbolic constants"; CCS #17; EC++ #2.
Where (the ones that recur or encode hardware/ABI facts):
- page size `0x1000` / `0xfff` (`src/process.cpp:659-660`) → `constexpr std::uint64_t page_size = 4096;`
- x86-64 max instruction length `15` (`src/disassembler.cpp:23`) → `constexpr std::size_t max_instruction_length = 15;`
- DR7 field layout: `1 << (free_space * 2)`, `<< (free_space * 4 + 16)`, `<< (free_space * 4 + 18)`, `0b11 << (index * 2) | 0b1111 << (index * 4 + 16)` (`process.cpp:748-753, 783`) → named `dr7_local_enable(slot)`, `dr7_rw_shift(slot)`, `dr7_len_shift(slot)` (see #31);
- `8` debug registers and the `4`/`5` aliases (`src/registers.cpp:86-88`, `process.cpp:535`), `6` syscall argument registers (`process.cpp:844`) → `std::size(arg_regs)`/a named constant; `sizeof(std::uint64_t) * i` is fine;
- `4096` "max path" (`src/target.cpp:453`) → `PATH_MAX`; `1024` (`src/pipe.cpp:44`) → `constexpr std::size_t read_chunk = 1024;`
- CLI layout: `32` default bytes, `16` per row (`tools/gsdb.cpp:691, 701-703`), `4` chars per byte token (`include/libgsdb/parse.hpp:75-81`), `13` (`test/tests.cpp:465` → `sizeof("Hello, gsdb!")`), `0b111` alignment mask (`registers.cpp:126` → `alignof(std::uint64_t) - 1`).
Not flagged: LEB128 masks `0x7f/0x80/0x40` (they *are* the encoding's definition and sit next to a comment), `0xcc` (already named `int3`), DWARF header size `11` (already named `header_size`).
Why: each repeated literal is a fact about x86-64 or DWARF that a reader should be able to search for by name; the DR7 arithmetic in particular is where a typo would go unnoticed.
Fix: add `include/libgsdb/detail/x64.hpp` with `inline constexpr` constants (`page_size = 4096`, `max_instruction_length = 15`, `debug_register_count = 8`, `syscall_arg_count = 6`) and three `constexpr` DR7 helpers (`dr7_enable_bit(slot)`, `dr7_rw_shift(slot)`, `dr7_len_shift(slot)`); replace the literals at the sites above. In the CLI, `constexpr std::size_t default_read_bytes = 32, bytes_per_row = 16;` at file scope; `sizeof("Hello, gsdb!")` in the test.

### 30. Signed/unsigned mixing centred on `virt_addr` arithmetic — should
Rules: CG ES.100 "Don't mix signed and unsigned arithmetic"; CG ES.102 "Use signed types for arithmetic"; CG ES.106 "Don't try to avoid negative values by using `unsigned`"; CG ES.104 "Don't underflow"; CG ES.46 "Avoid lossy (narrowing, truncating) arithmetic conversions"; `cppcoreguidelines-narrowing-conversions` (33 hits).
Where:
- `virt_addr::operator+/-/+=/-=(std::int64_t)` and `file_addr` likewise (`include/libgsdb/types.hpp:26-39, 90-103`) while `addr_` is `std::uint64_t` and every caller passes `std::size_t`/`std::uint64_t` (`process.cpp:664, 684, 694, 705, 710`; `dwarf.cpp:696, 724-748, 1168, 1198-1199`; `elf.cpp:143-144`) — 20 of the 33 narrowing warnings come from this one signature;
- `int chars_read` ← `ssize_t` (`pipe.cpp:45, 49`); `auto n_bytes = 32;` (`int`) later assigned a `std::size_t` (`gsdb.cpp:691-695`); `std::size_t high = count - 1;` underflows for an empty table (`dwarf.cpp:1610`); `for (int i = 0; i < 8; ++i)` indexing `u_debugreg[i]` (`process.cpp:535-546`).
Why: the address types exist to make address arithmetic safe; taking a signed offset and adding it to an unsigned value converts at every call site. Choose one integer model and make it visible.
Fix: give `virt_addr`/`file_addr` two overloads (`std::uint64_t` for sizes/forward offsets, `std::ptrdiff_t` for deltas) or a single `std::int64_t` with explicit `static_cast`/`gsl::narrow` at the boundaries; `ssize_t chars_read`; `std::size_t n_bytes = 32;`; guard `count == 0`. Turn on `-Wconversion -Wsign-conversion` (#61) so the compiler keeps this honest.

### 31. Bit manipulation performed on signed `int` — should
Rules: CG ES.101 "Use unsigned types for bit manipulation"; CG ES.46; CG ES.41 "If in doubt about operator precedence, parenthesize" (17 `math-missing-parentheses` hits, all in these expressions).
Where: `src/process.cpp:748-753` (`auto enable_bit = (1 << (free_space * 2));` … `auto clear_mask = (0b11 << (free_space * 2)) | (0b1111 << (free_space * 4 + 16));` — all `int`), `:783` (same for `clear_hardware_stoppoint`), `src/breakpoint_site.cpp:82, 108` (`data & ~0xff` — `~0xff` is a negative `int` sign-extended to `std::uint64_t`).
Why: for slot 3, `0b1111 << 28` produces a negative 32-bit `int` (well-defined only since C++20, and only by accident of two's complement); `~clear_mask` is then a *positive* `int` that, converted to `std::uint64_t`, clears every bit above bit 27 of DR7 — correct today only because slot 3's fields are the topmost. The code works, but for the wrong reasons and the reader cannot tell.
Fix: do all DR7 arithmetic in `std::uint64_t`: `constexpr std::uint64_t one = 1; auto enable_bit = one << (slot * 2);` … `auto clear_mask = (std::uint64_t{0b11} << (slot * 2)) | (std::uint64_t{0b1111} << (slot * 4 + 16));`; `data & ~std::uint64_t{0xff}`. Combine with the named constants from #29.

### 32. Casts: avoidable `reinterpret_cast`s, remote pointers typed as local pointers, `(void)` discards — should
Rules: CG ES.48 "Avoid casts"; CG ES.49 "If you must use a cast, use a named cast"; CG P.4 "Ideally, a program should be statically type safe"; CG C.90 "Rely on constructors and assignment operators, not `memset` and `memcpy`"; CG SL.con.4 "don't use `memset` or `memcpy` for arguments that are not trivially-copyable"; CCS #91-#92.
Where (24 `reinterpret_cast`s; the boundary ones in `bit.hpp`, `pipe.cpp`, `elf.cpp:48`, `process.cpp:42, 485, 662, 686, 927` are legitimate byte/OS-API conversions and are fine as long as they stay concentrated):
- remote addresses stored in *local* pointer types: `debug->r_map` (`link_map*`) and `entry.l_name` (`char*`) are addresses in the inferior, converted with `reinterpret_cast<std::uint64_t>(entry_ptr)` (`src/target.cpp:443, 450`). A `link_map*` that must never be dereferenced is a type lie; read the struct into a local POD with `std::uint64_t` fields (or `virt_addr` members) instead;
- `reinterpret_cast<std::byte*>(data_.u_debugreg + i)` followed by `from_bytes<std::uint64_t>` to obtain a value that is already a `std::uint64_t` (`src/registers.cpp:90-91`) → `data_.u_debugreg[i]`;
- `std::copy(bytes..., reinterpret_cast<std::byte*>(vec.data()))` to fill vectors of trivially-copyable structs (`src/elf.cpp:78-80, 170-172`; `src/target.cpp:386-388`) → `std::memcpy(vec.data(), src, n)` or a `copy_pods_from_bytes<T>(span, std::vector<T>&)` helper with a `static_assert(std::is_trivially_copyable_v<T>)`;
- `(void)parse_eh_frame_pointer_with_base(...)`, `(void)cur.u32();` (`src/dwarf.cpp:313, 379, 554`) → `std::ignore = ...;` (C++23-blessed) or `[[maybe_unused]] auto`;
- `from_bytes<To>` has no `static_assert(std::is_trivially_copyable_v<To>)` (`include/libgsdb/bit.hpp:20-25`), so `read_memory_as<T>` silently accepts any `T` (see #41).
Why: keeping all type punning behind two or three named helpers (`from_bytes`, `as_bytes`, `to_string_view`) is the right structure — the project mostly does this — so the goal is to route the stragglers through them and to stop representing remote memory with local pointer types.
Fix: (a) in `target.cpp`, read the rendezvous/link-map records into local structs whose pointer fields are `std::uint64_t` (`struct remote_link_map { std::uint64_t l_addr, l_name, l_ld, l_next, l_prev; }` via `read_memory_as`), so no `reinterpret_cast` is needed; (b) `data_.u_debugreg[i]` directly in `registers::flush`; (c) add `template <class T> requires std::is_trivially_copyable_v<T> std::vector<T> vector_from_bytes(span<const std::byte>)` to `bit.hpp` and use it in `elf.cpp`/`target.cpp`; (d) `std::ignore = expr;` for the three discarded values; (e) the `static_assert` in `from_bytes` (see #41).

### 33. Variables that shadow type names (and one parameter) — should
Rules: CG ES.12 "Do not reuse names in nested scopes"; CG NL.19 "Avoid names that are easily misread"; `-Wshadow`.
Where: `children_range(die die)` (`include/libgsdb/dwarf.hpp:554`); `parse_compile_unit(gsdb::dwarf& dwarf, ...)` / `parse_compile_units(gsdb::dwarf& dwarf, ...)` (`src/dwarf.cpp:437-439, 463-464`); `auto elf = ...` (`dwarf.cpp:1004, 1182, 1390, 1600`); `auto& dwarf = ...` (`src/breakpoint.cpp:101`, `src/stack.cpp:82`, `src/target.cpp:155`); `std::vector<gsdb::die> stack;` (`dwarf.cpp:1553`) and `auto stack = inline_stack_at_pc();` **inside `class stack`'s own member function** (`src/stack.cpp:27`); `auto [name, entry] = pair;` shadowing the `name` parameter (`dwarf.cpp:1296`); `auto pipe = popen(...)` under `using namespace gsdb;` shadows `gsdb::pipe` (`test/tests.cpp:32, 61`).
Why: `elf`, `dwarf`, `stack`, `pipe`, `die` are the project's five most important type names; using them as variable names makes `dwarf.cfi()` vs `dwarf::cfi()` a matter of context and blocks adding a static member call later.
Fix: `obj`/`elf_file`, `dw`/`debug_info`, `frames`/`inline_stack`, `proc_pipe`, `parent_die`; enable `-Wshadow` (#61).

### 34. Commented-out code and dead statements — should
Rules: CG NL.1 "Don't say in comments what can be clearly stated in code"; CCS #0 (consistency); LLVM ("don't check in commented-out code").
Where: `include/libgsdb/target.hpp:67-75` (old Meyers const-overload), `:144` (`[[maybe_unused]] auto pid`), `:161` (`// stack stack_;`); `include/libgsdb/process.hpp:277`; `src/process.cpp:911` (`// resume();`); `src/target.cpp:239` (commented condition inside a live `if`), `:344-357` (`elf_name` never used + commented `__cxa_demangle`); `tools/gsdb.cpp:197-200`, `:218-230` (a complete previous implementation kept in a comment), `:923`; `src/breakpoint_site.cpp:41`; `src/dwarf.cpp:1816-1818` (comment references `sdb::`, a different project's namespace).
Why: git already remembers the old versions; kept alternatives make readers wonder which one is current.
Fix: delete; where a design alternative is worth recording, one line "previously X; replaced by deducing `this`" is enough.

### 35. Two spellings of the logical operators in the same files — should
Rules: CG NL.8 "Use a consistent naming style" (consistency generally); `readability-operators-representation`.
Where: `and`/`or`/`not` vs `&&`/`||`/`!` — `src/dwarf.cpp` 76 vs 20, `tools/gsdb.cpp` 11 vs 16, `src/target.cpp` 17 vs 12, `src/elf.cpp` 11 vs 4, `test/tests.cpp` 26 vs 8, `include/libgsdb/stoppoint_collection.hpp` 2 vs 5 (e.g. `gsdb.cpp:162` `&&` and `:363` `or` in adjacent functions).
Why: either form is fine; alternating within a file is the only thing the guidelines object to.
Fix: pick one (the codebase leans `and`/`or`), add `readability-operators-representation` with `BinaryOperators: 'and;or;not'` to `.clang-tidy`, and let `--fix` normalize.

### 36. Loops: index loops over parallel arrays, `for` without an increment, repeated calls in a `do-while` condition — consider
Rules: CG ES.71 "Prefer a range-`for`-statement to a `for`-statement when there is a choice"; CG ES.73 "Prefer a `while`-statement to a `for`-statement when there is no obvious loop variable"; CG ES.75 "Avoid `do`-statements"; CG ES.3.
Where: `src/process.cpp:844-847` (`for (auto i = 0; i < 6; ++i) sys_info.args[i] = regs.read_by_id_as<...>(arg_regs[i]);` → `std::ranges::transform(arg_regs, sys_info.args.begin(), ...)`); `src/target.cpp:287-288` (`for (auto frames = stack.frames().size(); stack.frames().size() >= frames;)` → `const auto frames = ...; while (stack.frames().size() >= frames)`); `do`-`while` ×8 (`dwarf.cpp:87, 114, 412, 420, 1373`; `target.cpp:131, 222`; `tests.cpp:917`) — the LEB128 and abbreviation-table ones are idiomatic decoders and can stay; the two stepping loops in `target.cpp:143-148, 254-256` call `line_entry_at_pc(tid)` three times per iteration (each call re-walks the line program) → hoist into `auto line = line_entry_at_pc(tid);` once per iteration.
Fix: `std::ranges::transform(arg_regs, sys_info.args.begin(), [&](auto r) { return regs.read_by_id_as<std::uint64_t>(r); });`; `const auto frame_count = stack.frames().size(); while (stack.frames().size() >= frame_count) { ... }`; in `step_in`/`step_over` factor the loop condition into a small `bool at_new_line(line_table::iterator line, line_table::iterator orig)` helper evaluated once per iteration; leave the LEB128 `do`-loops as they are.

### 37. Readability nits, clustered — consider
Rules: CG ES.78 "Don't rely on implicit fallthrough in `switch` statements"; CG ES.87 "Don't add redundant `==` or `!=` to conditions" / ToTW #141 (pick one bool-conversion style); CG ES.56 "Write `std::move()` only when you need to explicitly move an object to another scope" (and its inverse); `readability-else-after-return`.
Where: `case X: gsdb::error::send(...);` followed directly by the next `case` label with no `break` (`src/dwarf.cpp:750-752, 791-794, 1872-1880, 1970-1975`) — correct because `send` is `[[noreturn]]`, but it looks like fallthrough; end each with `std::unreachable();` or a `// [[noreturn]]` comment. Pointer-to-bool style mixes `if (!cu)` / `if (elf)` (28 sites) with `if (p != nullptr)` (`gsdb.cpp:973`, `breakpoint.cpp:...`) — choose one. `std::string func_name = "";` (`target.cpp:345`), `std::string_view sep = "";` (`gsdb.cpp:192`) — redundant initializers. `return pieces_result{pieces};` copies a local vector into the return value (`dwarf.cpp:2031`) → `std::move(pieces)`. `else` after `return` ×9 (`dwarf.cpp:1143, 1157, 1163, 1250`, `process.cpp:410, 896`, `gsdb.cpp:166, 403`) — LLVM early-exit style; optional.
Fix: after each `error::send(...)` inside a `switch`, add `std::unreachable();` (C++23) so the intent is explicit and `-Wimplicit-fallthrough` stays quiet; adopt `if (!p)` for pointers everywhere (matches the majority) and add `readability-implicit-bool-conversion.AllowPointerConditions: true` to `.clang-tidy`; drop the two `= ""` initializers; `return pieces_result{std::move(pieces)};`; let `readability-else-after-return --fix` handle the nine `else`s if you want the LLVM style.

### 5.5 Enums, constants, immutability

### 38. Enum arithmetic for debug registers repeated at six sites — should
Rules: CG Enum.4 "Define operations on enumerations for safe and simple use"; CG ES.3.
Where: `static_cast<register_id>(static_cast<int>(register_id::dr0) + i)` and variants — `src/process.cpp:535-537, 740-742, 767-768, 776-778, 790-791, 884-887`.
Why: the same two casts appear six times; a named operation documents the "DR0..DR7 are contiguous in the enum" assumption once and lets `-Wswitch` reasoning stay intact elsewhere.
Fix: in `register_info.hpp`: `constexpr register_id debug_register(int index) { return static_cast<register_id>(std::to_underlying(register_id::dr0) + index); }` (C++23 `std::to_underlying`), plus `static_assert(std::to_underlying(register_id::dr7) - std::to_underlying(register_id::dr0) == 7)`.

### 39. X-macro helper macros leak out of the `.inc` file and are unprefixed — should
Rules: CG ES.33 "If you must use macros, give them unique names"; CG ES.31 "Don't use macros for constants or 'functions'" (the X-macro itself is the documented exception); CG ES.32.
Where: `include/libgsdb/detail/registers.inc:6-24, 86-89, 131` define `GPR_OFFSET`, `DEFINE_GPR_64/32/16/8H/8L`, `FPR_OFFSET`, `FPR_SIZE`, `DEFINE_FPR`, `DR_OFFSET`; the includer (`register_info.hpp:15-17, 51-54`) `#undef`s only `DEFINE_REGISTER`, so the ten helpers stay defined in every translation unit that includes `register_info.hpp` (i.e. all of them).
Why: an unprefixed `DEFINE_FPR` or `DR_OFFSET` in a public header is a collision waiting for a third-party header; the fix is mechanical.
Fix: `#undef` every helper at the bottom of `registers.inc`, and prefix them `GSDB_` (or move the helper definitions into the includer next to `DEFINE_REGISTER`).

### 40. Enum and constant hygiene — consider
Rules: CG Enum.3 "Prefer class enums over 'plain' enums"; CG Enum.6 "Avoid unnamed enumerations"; CG ES.27 "Use `std::array` or `stack_array` for arrays on the stack"; CG Con.5 "Use `constexpr` for values that can be computed at compile time"; EMC++ #10, #15.
Where: `enum mode { none, some, all };` nested in `syscall_catch_policy` (`include/libgsdb/process.hpp:93`) — already used as `mode::none` everywhere, so `enum class` is a one-word change; `include/libgsdb/detail/dwarf.h` (571 lines of unnamed `enum { DW_TAG_... }` constants, 25+31 tidy hits) — they mirror the DWARF spec's integer constants and are compared against raw `std::uint64_t` fields (`abbrev.tag`, `attr.form`); typing them (`enum class dw_tag : std::uint16_t`) would also type `abbrev::tag`/`attr_spec::form` (P.4), a larger but valuable refactor; `inline constexpr const register_info g_register_infos[]` (`register_info.hpp:50`) → `inline constexpr auto g_register_infos = std::to_array<register_info>({...})` gives `.size()` and algorithms for free (the redundant `const` after `constexpr` can go either way); `const auto vdso_name = "linux-vdso.so.1";` inside a function (`src/target.cpp:461`) → `constexpr std::string_view vdso_name` at namespace scope.
Fix: `enum class mode { none, some, all };` (no call-site changes needed); `inline constexpr auto g_register_infos = std::to_array<register_info>({ ... })` with the X-macro body unchanged inside the braces; move `vdso_name` into the file's unnamed namespace as `constexpr std::string_view`. For `detail/dwarf.h`, if you take it on: one `enum class dw_tag : std::uint16_t`, `dw_at`, `dw_form`, `dw_op : std::uint8_t`, then change `abbrev::tag`/`attr_spec::attr`/`attr_spec::form` to those types and add `std::to_underlying` at the two decode sites — do it per enum, not all at once.

### 5.6 Templates & generic code

### 41. Unconstrained templates; type-punning helpers without `static_assert`s — should
Rules: CG T.10 "Specify concepts for all template arguments"; CG T.11 "Whenever possible use standard concepts"; CG SL.con.4; CG C.90; CG I.9.
Where: `stoppoint_collection::for_each(F f)` ×2 (`include/libgsdb/stoppoint_collection.hpp:71-74`), `elf_collection::for_each(F f)` ×2 (`elf.hpp:138-141`), `register_info_by(F f)` (`register_info.hpp:61-62`), `process::read_memory_as<T>` (`process.hpp:194-198`), `from_bytes<To>`, `as_bytes<From>`, `to_byte128<From>`, `to_byte64<From>` (`bit.hpp:20-49`), `to_integral<I>`, `to_float<F>` (`parse.hpp:15-16, 51-52`). The project already writes concepts well (`stoppoint_concept`, `format_join`'s `std::input_iterator`/`std::ranges::range` constraints), so the precedent exists.
Why: `read_memory_as<std::string>(addr)` and `from_bytes<std::vector<int>>(p)` compile today and are undefined behaviour; a constraint turns that into a compile error with a readable message. `for_each(F)` should say what it calls `F` with.
Fix:
```cpp
template <class To> requires std::is_trivially_copyable_v<To> To from_bytes(const std::byte* bytes);
template <class From> requires (std::is_trivially_copyable_v<From> && sizeof(From) <= sizeof(byte128)) byte128 to_byte128(From src);
template <std::invocable<Stoppoint&> F> void for_each(F f);
template <std::integral I> std::optional<I> to_integral(std::string_view sv, int base = 10);
template <std::floating_point F> std::optional<F> to_float(std::string_view sv);
```

### 42. Full specialization of a function template — should
Rules: CG T.144 "Don't specialize function templates"; CCS #66; EC++ (overloading vs specialization); note `std::byte` is not `std::integral`, so the constraint in #41 makes this explicit.
Where: `include/libgsdb/parse.hpp:41-49` (`template <> inline std::optional<std::byte> to_integral<std::byte>(std::string_view, int)`).
Why: function-template specializations do not participate in overload resolution and interact surprisingly with the primary template's overloads; the guideline is to overload or branch inside the primary.
Fix: `if constexpr (std::same_as<I, std::byte>) { ... }` inside the primary (relax the constraint to `std::integral<I> || std::same_as<I, std::byte>`), or a separate `to_byte(std::string_view)` function.

### 43. Hand-rolled `gsdb::span` on a C++23 code base — should
Rules: CG ES.1 "Prefer the standard library to other libraries and to 'handcrafted code'"; CG T.47 "Avoid highly visible unconstrained templates with common names"; CG P.4; CG I.13 "Do not pass an array as a single pointer".
Where: `include/libgsdb/types.hpp:59-77` and every `span<const std::byte>` use. Concrete cost: `test/tests.cpp:696-698` builds `{bytes, bytes + range_data.size()}` — an **8-byte** span over **64 bytes** of `std::uint64_t` data (element count used as byte count); the test passes only because `range_list::iterator` never checks `data_.end()`. With `std::as_bytes(std::span{range_data})` the length is correct by construction.
Why: the local `span` lacks `data()`, `subspan()`, `size_bytes()`, `empty()`, a const `operator[]`, and CTAD; `std::span<const std::byte>` has all of them plus `std::as_bytes`/`std::as_writable_bytes`, and the name `span` in `namespace gsdb` will collide with `std::span` the day someone writes `using namespace std;` in a test.
Fix: `using std::span;` is not enough because of the two-pointer constructor and the `vector<U>` converting constructor; replace uses with `std::span<const std::byte>` (its `(first, last)` constructor covers `{start, end}`), delete `gsdb::span`, and fix the test with `std::as_bytes`.

### 5.7 Error handling

### 44. `errno` read after work that can change it — should
Rules: CG E.28 "Avoid error handling based on global state (e.g. `errno`)"; CERT ERR30-C (analogue).
Where: `include/libgsdb/error.hpp:20-23` — `throw error(prefix + ": " + std::strerror(errno));` evaluates `prefix + ": "` (allocation) before `std::strerror(errno)`. Order of evaluation of `operator+` operands is unspecified pre-C++17 and left-to-right since; either way, intervening library calls may clobber `errno`.
Fix: `const int err = errno;` as the first statement of `send_errno`, then `std::strerror(err)` (or `std::system_error{err, std::generic_category(), prefix}` — which also gives callers the code, see #48).

### 45. Library code prints to `std::cerr` and calls `std::terminate` — should
Rules: CG E.2 "Throw an exception to signal that a function can't perform its assigned task"; CERT ERR50-CPP "Do not abruptly terminate the program"; CG SL.io (a library should not own the process's stderr).
Where: `src/registers.cpp:109-112` (`std::cerr << "... mismatched register and value sizes!"; std::terminate();`), which is also the only reason `registers.cpp` includes `<iostream>`.
Why: every other invalid-argument path in `libgsdb` throws `gsdb::error` and lets the CLI decide; this one kills the debugger (and the inferior with it). The CLI's `parse_register_value` guarantees the sizes match, so this branch is a contract check — `error::send("...")` keeps the contract and the behaviour policy.
Fix: replace the two lines with `gsdb::error::send("registers::write: value size exceeds register size");` and delete the `<iostream>` include from `registers.cpp`.

### 46. `catch` clauses: by non-const reference, unused names, catch-all — should
Rules: CG E.15 "Throw by value, catch exceptions from a hierarchy by reference" (const&); CG E.17 "Don't try to catch every exception in every function"; CG E.31 "Properly order your catch-clauses"; CERT ERR61-CPP.
Where: `tools/gsdb.cpp:373` and `:423` (`catch (gsdb::error& err)`; `err` unused at 373); `:407-408` (`catch (...) {}` around `std::optional::value()` calls, then a generic "Invalid format!").
Fix: `catch (const gsdb::error&)`; replace the catch-all with `catch (const std::bad_optional_access&)` or, better, test the `optional` before `.value()` and drop the `try` entirely.

### 47. Unchecked or unsafe conversions of external input — should
Rules: CG SL.io.2 "When reading, always consider ill-formed input"; CERT ERR62-CPP "Detect errors when converting a string to a number"; CERT STR37-C (analogue: character-classification functions take `unsigned char`); CG I.5.
Where: `std::atoi(argv[2])` for the `-p <pid>` argument (`tools/gsdb.cpp:164`) — `gsdb -p abc` attaches to PID 0 → "INVALID PID" is caught later only by luck; `isdigit(syscall[0])` on a `char` (and on a possibly empty string) (`gsdb.cpp:864`); `std::regex_search(data, groups, map_regex);` result ignored before `groups[2]` (`test/tests.cpp:120`); `mkdtemp(tmp_dir)` return value ignored (`src/target.cpp:51`).
Fix: `gsdb::to_integral<pid_t>(argv[2])` (already in the project) with an error message; `!syscall.empty() && std::isdigit(static_cast<unsigned char>(syscall[0]))`; `if (!std::regex_search(...)) continue;`; check `mkdtemp` for `nullptr` and `error::send_errno`.

### 48. Error-policy consistency — consider
Rules: CG E.14 "Use purpose-designed user-defined types as exceptions (not built-in types)"; CG E.27 "If you can't throw exceptions, use error codes systematically"; CG F.46 "`int` is the return type for `main()`"; CG I.6; CPL.3.
Where/what:
- one exception type (`gsdb::error`) carries every failure as a string, so callers cannot distinguish "not found" from "OS call failed" from "bad user input"; the CLI currently needs no distinction, but `to_integral`/`parse_vector` already mix `std::optional` (absent) with throwing (invalid) — fine, just document the rule "optional = absent, exception = invalid";
- `std::stoi` in `populate_existing_threads` (`src/process.cpp:941`) throws `std::invalid_argument`, not `gsdb::error`;
- `exit(-1)` in the forked child (`src/process.cpp:43`) — after `fork()` a child should `_exit()` (no atexit handlers, no double flush of inherited stdio buffers), and `EXIT_FAILURE` beats `-1` (also `return -1;` in `main`, `gsdb.cpp:1022`);
- errors are printed to `std::cout` (`gsdb.cpp:992, 1032`) while help goes to `std::cerr`; swap;
- `assert(elf_ == other.elf_)` in `file_addr` comparisons (`include/libgsdb/types.hpp:112-125`) and `assert(elf_ && ...)` (`src/types.cpp:6`) are contract checks that vanish in release builds — decide whether cross-ELF comparison is a precondition (document it) or an error (throw).
Fix: write the policy down in `error.hpp` ("`std::optional` = absent, `gsdb::error` = invalid input/OS failure, `assert` = internal invariant") and apply it: wrap `std::stoi` in `to_integral<pid_t>(...).value_or(...)` + `error::send`; `_exit(EXIT_FAILURE)` in the child and `return EXIT_FAILURE;` in `main`; `std::print(stderr, "{}\n", err.what())` at the two catch sites; keep the `file_addr` asserts but state the "same ELF" precondition in a comment on the class. Optionally derive `os_error`/`parse_error`/`not_found` from `gsdb::error` when a caller first needs to tell them apart.

### 5.8 Source files & headers

### 49. Include style is mixed in every file; one implicit include — should
Rules: CG SF.12 "Prefer the quoted form of `#include` for files relative to the including file and the angle bracket form everywhere else"; CG SF.10 "Avoid dependencies on implicitly `#include`d names"; Google:Names and Order of Includes.
Where: 18 of 20 files with project includes use both `#include <libgsdb/x.hpp>` and `#include "libgsdb/y.hpp"` (e.g. `include/libgsdb/process.hpp:14-15` angle vs `:23-27` quoted; `src/dwarf.cpp:11-12` vs `:22-27`; `src/target.cpp:10-11` vs `:18-25`); clang-format then sorts them into two separate blocks, which is why the split looks intentional but isn't. `std::format` is used at `src/process.cpp:544` with no `#include <format>` (it compiles via libstdc++'s transitive includes).
Fix: quoted for project headers, angle for system/std (the Google convention the formatter is configured for), one pass with sed; add `<format>`. Optionally set `IncludeCategories` in `.clang-format` so the order is enforced.

### 50. Headers that include heavy headers where a forward declaration suffices — should
Rules: CG SF.9 "Avoid cyclic dependencies among source files"; CG SF.11 "Header files should be self-contained"; EC++ #31 "Minimize compilation dependencies between files"; CCS #22.
Where: `include/libgsdb/dwarf.hpp:19` includes `process.hpp` although `process` appears only as `const process&` in declarations (`registers.hpp` at `:20` is needed — `registers` is returned by value); `include/libgsdb/elf.hpp:16` includes `dwarf.hpp` although `dwarf` is only held by `std::unique_ptr<dwarf>` (destructor is out-of-line in `elf.cpp`) and returned by reference; `include/libgsdb/disassembler.hpp:4` includes `process.hpp` for a `process*` member and a `process&` parameter. Net effect: `process.hpp` (the largest header) is pulled into every TU, and `elf.hpp ↔ dwarf.hpp ↔ process.hpp` form a soft cycle that `types.hpp:14-18` already has to break with forward declarations.
Fix: `class process;` / `class dwarf;` forward declarations in the three headers; include the full headers in the `.cpp` files. `stack.hpp` legitimately needs `dwarf.hpp` (`die` by value).

### 51. Header naming and C headers — consider
Rules: CG SF.1 "Use a `.cpp` suffix for code files and `.h` for interface files if your project doesn't already follow another convention" / NL.27; `modernize-deprecated-headers`; Google (`#endif  // GUARD`).
Where: `include/libgsdb/detail/dwarf.h` is the only `.h` among `.hpp`s (it is a C-compatible constants table — fine if intentional; say so in a comment or rename); `test/targets/anti_debugger.cpp:1` `#include <signal.h>` → `<csignal>`; `#endif` lines carry no `// GSDB_X_HPP` comment (Google style adds one; optional).
Fix: add `// C-compatible DWARF constant table; kept as .h on purpose` at the top of `detail/dwarf.h` (or rename to `.hpp` and update the three includers); `#include <csignal>`; if you want the `#endif` comments, a one-line sed over `include/` adds them.

### 5.9 Standard library

### 52. `count()` followed by `at()`/`emplace()` — double lookups and pre-C++20 idioms — should
Rules: CG P.9 "Don't waste time or space"; CG ES.1; ESTL #45; `readability-container-contains`.
Where: `src/syscalls.cpp:27-30` (`count(name) != 1` then `at(name)`), `src/elf.cpp:98-102` (`count(name) == 0` then `at(name)`), `src/dwarf.cpp:885-889` (`!count(offset)` → `emplace` → `at`), `:1581-1593` (`count(offset)` → `at`; then `emplace` + `at`), `src/process.cpp:254` (`!threads_.count(tid)`).
Fix: `if (auto it = map.find(key); it != map.end()) return it->second;`; `auto [it, inserted] = abbrev_tables_.try_emplace(offset, ...)` (also avoids parsing when present); `contains()` for pure existence checks.

### 53. Output: `std::endl`, three output mechanisms in one program, per-character `<<` — should
Rules: CG SL.io.50 "Avoid `endl`"; CG SL.io.3 "Prefer `iostream`s for I/O" (read today as: one consistent facility — `<print>` on C++23); CG SL.io.1 "Use character-level input only when you have to"; `performance-avoid-endl`.
Where: `tools/gsdb.cpp:781` (`std::cout << std::endl;`), `print_source` (`gsdb.cpp:745-782`) reads and writes one `char` at a time and mixes `std::cout << c` with `std::print`; the file uses `std::print` (26), `std::cerr <<` (17), `std::cout <<` (4).
Fix: `std::print`/`std::println` everywhere, `std::print(stderr, ...)` for help/errors; in `print_source` read lines with `std::getline` and print each with `std::println("{} {:>{}} {}", arrow, n, width, line)`; drop `std::endl` (`'\n'` and, if needed, `std::cout.flush()`).

### 54. Compiler intrinsic where the standard has the function — should
Rules: CG P.2 "Write in ISO Standard C++"; CG ES.1.
Where: `src/process.cpp:883` (`__builtin_ctzll(status)`).
Fix: `std::countr_zero(status)` from `<bit>` (C++20); same semantics, portable, and `constexpr`.

### 55. Standard-library idiom nits — consider
Rules: ESTL #4 "Call `empty()` instead of checking `size()` against zero"; CG NL.8 (consistent qualification); C++20/23 replacements (EMC++ addenda); `modernize-use-starts-ends-with`.
Where: `data.size() > 0` (`src/process.cpp:480`), `func_name != ""` (`tools/gsdb.cpp:298`) → `!empty()`; `args[2].find("0x") == 0` (`gsdb.cpp:489`) → `starts_with("0x")`; `split()` via `std::stringstream` (`gsdb.cpp:176-186`) → `std::views::split` + `std::ranges::to<std::vector<std::string>>` (C++23); unqualified `memcpy` (`src/watchpoint.cpp:54`), `uint64_t`/`size_t` (`src/dwarf.cpp:90, 117`; `test/tests.cpp:392, 400, 881`) vs `std::`-qualified everywhere else; `std::unordered_multimap<std::string, index_entry>` (`dwarf.hpp:493`) could take `std::string_view` lookups with a transparent hasher (`struct sv_hash { using is_transparent = void; ... }` + `std::equal_to<>`), which is what unlocks `find_functions(std::string_view)` in #9.
Fix: `!data.empty()`, `!func_name.empty()`, `args[2].starts_with("0x")`; `std::memcpy`, `std::uint64_t`, `std::size_t` at the listed lines; for `split`, `return str | std::views::split(delimiter) | std::ranges::to<std::vector<std::string>>();`; for the index, `std::unordered_multimap<std::string, index_entry, string_hash, std::equal_to<>>` with a ten-line `string_hash` (`is_transparent`, hashes `std::string_view`).

### 5.10 Concurrency & signals

### 56. SIGINT handler dereferences a C++ object through a global pointer — should
Rules: CG I.2 "Avoid non-const global variables"; CG CP.1 / CP.200 (signals are the one concurrency in this program); CERT SIG30-C/SIG31-C (analogues: call only async-signal-safe functions, access only `volatile sig_atomic_t` or lock-free atomics from a handler); CERT MSC54-CPP "A signal handler must be a plain old function".
Where: `tools/gsdb.cpp:49-56` (`gsdb::process* g_gsdb_process`; `kill(g_gsdb_process->pid(), SIGSTOP)`), `:1027-1028` (`signal(SIGINT, handle_sigint)`).
Why: `pid()` is an inline getter today, so the handler is *practically* safe; the guideline asks that this not depend on the implementation of a class the handler cannot see. `std::signal` also has implementation-defined semantics (`SA_RESTART`, handler reset) that `sigaction` pins down.
Fix: `std::atomic<pid_t> g_inferior_pid{0};` (lock-free on x86-64, `static_assert(std::atomic<pid_t>::is_always_lock_free)`) or `volatile std::sig_atomic_t`; set it after `attach()`; handler becomes `kill(g_inferior_pid.load(), SIGSTOP);`; install with `sigaction` and `SA_RESTART`.

### 57. Document that the caches are single-threaded — consider
Rules: EMC++ #16 "Make `const` member functions thread safe"; CG CP.1 "Assume that your code will run as part of a multi-threaded program".
Where: `mutable` caches mutated in `const` functions — `call_frame_information::cie_map_` (`include/libgsdb/dwarf.hpp:91-92`), `line_table::file_names_` (`:343`), `dwarf::function_index_` (`:493`).
Fix: one class-level comment ("not thread-safe; the debugger is single-threaded") or, if that ever changes, `std::call_once`/a mutex around `index()`.

### 5.11 Naming, layout, comments

### 58. Accessor naming: `get_x()` and `x()` coexist, sometimes in the same class — should
Rules: CG NL.8 "Use a consistent naming style"; Google:Function Names; CCS #0.
Where: 37 `get_*` accessors vs bare accessors (`pid()`, `state()`, `id()`, `address()`, `cfa()`, `frames()`, `lines()`, `root()`, `data()`, `path()`, `mode()`, `size()`): `process::get_pc()` vs `process::pid()`; `elf::get_header()` vs `elf::path()`; `stack::get_pc()` vs `stack::regs()`; `pipe::get_read()`; `syscall_catch_policy::get_mode()` vs `watchpoint::mode()`; `target::get_process()`/`get_elf()`/`get_main_elf()`/`get_elves()` vs `target::breakpoints()`/`threads()`.
Why: readers cannot predict a name; the split does not follow any rule (cheap getter vs computation).
Fix: bare nouns for cheap accessors (`header()`, `pc()`, `read_fd()`, `mode()`), verbs for work (`find_functions`, `read_memory`, `section_containing(addr)`, `symbol_at(addr)`). Where the noun collides with the type (`process()`), `proc()`/`inferior()` are common.

### 59. Comments that teach C++ rather than explain the code; stale markers — should
Rules: CG NL.1 "Don't say in comments what can be clearly stated in code"; CG NL.2 "State intent in comments"; CG NL.3 "Keep comments crisp".
Where (≈40 blocks; representative):
- language tutorials: `include/libgsdb/register_info.hpp:48-49, 72-73` (what `inline` does — twice), `include/libgsdb/stoppoint_collection.hpp:30-33, 37, 54-56, 93-94, 123-125, 155-156` (what a template/`conditional_t`/`typename`/`const_cast`/`**` is), `include/libgsdb/dwarf.hpp:126-127, 485-492, 555-563, 572-582, 595-597` (what `mutable` is, the five iterator categories, how post-increment works), `include/libgsdb/elf.hpp:101-110` (what a multimap is), `src/pipe.cpp:47-48, 54` (what `::` and "raw pointers are valid iterators" mean), `src/process.cpp:633-643` (the `iovec` man page), `src/elf.cpp:205` (`// structured bindings`), `src/registers.cpp:101-102`, `test/tests.cpp:387-391` (the same init-capture explanation pasted twice);
- stale/orphaned: `// why?` (`src/dwarf.cpp:76`), `// So one` (`:488`), `// FIXME` with no explanation (`src/stack.cpp:124`), a comment referring to `sdb::` (`src/dwarf.cpp:1816-1817`), "Meyers const-overload" block that is now dead code (`target.hpp:67`);
- comments that restate the next line: `// initialize the breakpoint sites for the breakpoint` above `resolve();`, `// wait for it to terminate` above `waitpid`.
Why: the *why*-comments in this codebase are excellent (the `int3` PC rewind essay in `breakpoint_site.cpp:44-60`, the DR7 per-thread note in `process.cpp:760-763`, the x64 `SI_KERNEL` remark) — the language notes bury them. Learning notes belong in `.claude/`/`docs/` (the repository already keeps walkthrough docs there).
Fix: delete the tutorials, keep every hardware/ABI/why comment, resolve or expand the three stale markers.

### 60. Identifier spelling and small layout points — consider
Rules: CG NL.19 "Avoid names that are easily misread"; CG NL.8; CG NL.16 "Use a conventional class member declaration order".
Where: member `fde_has_augmentaion` (typo in an *identifier*, `include/libgsdb/dwarf.hpp:35`, used at `src/dwarf.cpp:356`); local `chilren` (`dwarf.cpp:1560`); parameter named like a member `iterator(const line_table* table_)` (`dwarf.hpp:379`); comment typos (`Everyt`, `alredy`, `wsa`, `memeory`, `parant`, `itsef`, `statis`, `doen`, `requestd`, `rebgin`, `leaset`, `reister`, `x6` — a spell-checker pass); member order: `process() = delete;` block sits after public member functions (`process.hpp:132-136`) while every other class puts special members first; `breakpoint_site` interleaves `using id_type` between deleted special members and accessors (`breakpoint_site.hpp:16-35`).
Fix: rename `fde_has_augmentaion` → `fde_has_augmentation` (two sites) and `chilren` → `children`; rename the parameter to `table`; run a spell-checker (`codespell include src tools test`) over comments; move the `= delete` block in `process` to the top of the public section and group `breakpoint_site` as types → special members → operations → queries, matching the other classes.

### 5.12 Build & tooling

### 61. Warning set is the 2005 baseline — should
Rules: CCS #1 "Compile cleanly at high warning levels"; Turner (recommended flags); CG P.12 "Use supporting tools as appropriate"; EC++ #53.
Where: `CMakeLists.txt:31` (`-Wall -Wfatal-errors -Wextra -Werror -g -O1`).
Why: `-Wshadow` would have caught #33, `-Wconversion -Wsign-conversion` #30, `-Wold-style-cast` guards #32, `-Wnon-virtual-dtor`/`-Woverloaded-virtual` protect #19/#20, `-Wimplicit-fallthrough` clarifies #37, `-Wpedantic` enforces P.2 (#54). The project already accepts `-Werror`, so adding flags is cheap now and expensive later.
Fix: add `-Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wpedantic -Wnull-dereference -Wimplicit-fallthrough -Wcast-align -Wuseless-cast -Wduplicated-cond -Wlogical-op` (GCC set) behind a `GSDB_STRICT_WARNINGS` option first, clean the baseline, then default it on. Consider dropping `-Wfatal-errors` in CI (it hides all but the first error — today it hid two of the three WIP blockers).

### 62. No `.clang-tidy`; a dead dependency; two switches for tests — should
Rules: CG P.12; Turner (static analysis in the build); CCS #2 "Use an automated build system".
Where: no `.clang-tidy` at the root although `compile_commands.json` is generated and symlinked; `CMakeLists.txt:86-90` `find_package(GTest REQUIRED)` + `include(GoogleTest)` are unused (CLAUDE.md already notes this — this review confirms nothing links it); `option(ENABLE_TESTING ...)` (`:78`) coexists with CTest's `BUILD_TESTING` (`:99`), and only the latter gates `add_subdirectory(test)`.
Fix: commit the `.clang-tidy` from §7; delete the GTest lines (removes a hard `find_package` failure for anyone without GTest); drop `ENABLE_TESTING` or make it set `BUILD_TESTING`.

### 63. CMake modernization — consider
Rules: Turner (modern CMake); CG A.1-A.4 (architecture hygiene).
Where: `include_directories(${PROJECT_SOURCE_DIR})` (`CMakeLists.txt:95`) is a directory-scoped include applied to every target while the library already declares target-scoped includes; `set(CMAKE_C_COMPILER gcc)` (`:26`) inside the project (compilers belong to the toolchain/environment — the Nix shell already sets them); `-g -O1` hard-coded for every build type (`:31`) instead of per-config flags; `include(GNUInstallDirs)` repeated in three lists; no sanitizer or `CMakePresets.json` configuration (`-fsanitize=address,undefined` would be a natural companion to the ptrace tests).
Fix: delete `include_directories(...)` (the library's `target_include_directories` already covers consumers) and `set(CMAKE_C_COMPILER gcc)`; replace the global `-g -O1` with `$<$<CONFIG:Debug>:-g -O0>` / `$<$<CONFIG:RelWithDebInfo>:-g -O2>` generator expressions (or just rely on `CMAKE_BUILD_TYPE`); keep one `include(GNUInstallDirs)` in the root; add an `option(GSDB_SANITIZE ...)` that appends `-fsanitize=address,undefined -fno-omit-frame-pointer` to compile and link options, and a `CMakePresets.json` with `debug`, `release`, `asan` presets.

---

## 6. What is already in good shape

- **Ownership model**: every owning relationship is a `std::unique_ptr` (`stoppoint_collection`, `compile_units_`, `elves_`, `target::process_`); copy is deleted on all resource types except `pipe`; factories return `unique_ptr`. `std::exchange` is used correctly for release semantics.
- **Strong address types** (`virt_addr` / `file_addr` / `file_offset`) are a model of CG P.1/I.4 — most debuggers pass `uint64_t` everywhere.
- **Modern standard usage**: concepts (`stoppoint_concept`, `std::input_iterator`/`std::sentinel_for` in `format_join`), deducing `this` (`target::get_stack`), `<print>`/`<format>`, `std::optional`/`std::variant` for absent/alternative values, `std::from_chars`, structured bindings, `if` with initializer, `enum class` for all public enums, init-captures in lambdas, `[[noreturn]]` on the error senders.
- **Encapsulation of the messy parts (CG P.11)**: all `ptrace` calls live in `process.cpp`/`breakpoint_site.cpp`; all DWARF byte decoding goes through one `cursor`; all type punning through `bit.hpp`.
- **Header hygiene**: consistent `GSDB_*_HPP` guards, no `using namespace` in any header, unnamed namespaces for internal helpers in every `.cpp`, X-macros `#undef`'d by their includers.
- **Formatting and warnings**: 0 clang-format drift; `-Wall -Wextra -Werror` enforced.
- **Tests** exist for every layer and use a real inferior; the `bool close_on_exec = false; pipe channel(close_on_exec);` idiom is exactly the readability the `bool`-parameter guideline asks for.
- **Why-comments** on hardware/kernel behaviour (int3 PC rewind, `SI_KERNEL` vs `TRAP_BRKPT`, per-thread debug registers, `PTRACE_O_TRACESYSGOOD`) are genuinely valuable and should be kept.

---

## 7. Proposed `.clang-tidy` for this project

Baseline tuned to the histogram in §4 and the house rules (X-macros allowed, `ptrace` vararg calls unavoidable, byte-parser pointer arithmetic confined to `cursor`). Start with `WarningsAsErrors: ''`, clean the baseline, then promote.

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
  bugprone-easily-swappable-parameters, bugprone-reserved-identifier, bugprone-macro-parentheses,
  -modernize-use-trailing-return-type,
  -readability-identifier-length,
  -readability-magic-numbers, -cppcoreguidelines-avoid-magic-numbers,
  -readability-function-cognitive-complexity,
  -readability-qualified-auto,
  -misc-non-private-member-variables-in-classes, -cppcoreguidelines-non-private-member-variables-in-classes,
  -misc-include-cleaner,
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

Run: `clang-tidy -p build src/*.cpp tools/gsdb.cpp test/tests.cpp` (or the skill's `scripts/run-clang-tidy.sh .`).

---

## 8. Suggested order of work

1. **Lifetime fixes** — #2 (owning demangled names), #1 (RAII members in `elf`), #3 (`pipe` copy ops), #7 (`[this]`). Small diffs, highest value; #1 and #4 share the `unique_fd`/`c_string_ptr` helpers.
2. **Compiler-assisted sweep** — add the warning flags (#61) and `.clang-tidy` (#62); fix what they surface: shadowing (#33), narrowing (#30/#31), initialization (#16/#17/#28), `explicit` (#11), unused params (#12), catch clauses (#46), `contains`/`try_emplace` (#52), `endl`/print consistency (#53), `and`/`or` normalization (#35 via `--fix`).
3. **API shape** — bool parameters → enums (#8), parameter passing (#9/#10), `disassembler::instruction` public + `const path` (#14), `up()/down()` contracts (#13), `describe()` instead of `dynamic_cast` (#20), `add_site()` helper and private data in `breakpoint` (#18/#19).
4. **Modernization** — `std::span` (#43 + the test fix), `<=>` (#23), deducing `this` for the const overloads (#22), constraints and `static_assert`s (#41/#42), `std::countr_zero` (#54), `debug_register()` helper (#38), `.inc` macro hygiene (#39).
5. **Housekeeping** — include style (#49/#50), dead code and tutorial comments (#34/#59), accessor naming (#58, do it once, mechanically), CMake cleanup (#62/#63).

---

## Appendix A — Grep hotspot details

- `reinterpret_cast` (24): `bit.hpp:13, 29, 34` · `elf.cpp:48, 80, 87, 121, 172` · `process.cpp:42, 485, 662, 686, 927` · `target.cpp:61, 387, 443, 450, 454` · `dwarf.cpp:74, 1863` · `registers.cpp:90` · `pipe.cpp:53` · `tests.cpp:100, 696`.
- `const_cast` (5): `stoppoint_collection.hpp:109, 126, 162, 177` · `process.cpp:151`.
- `dynamic_cast` (3): `gsdb.cpp:453, 457, 461`.
- `new` (5, all private-ctor factories): `target.cpp:319, 324, 330` · `process.cpp:583, 594, 809`.
- `free(` (4): `elf.cpp:186` · `gsdb.cpp:978, 985` · `tests.cpp:80, 86`.
- `[[maybe_unused]]` (19): listed in #12.
- `do {` (8): listed in #36.
- `[=]`/`[&]` (20 lambdas): `stoppoint_collection.hpp:103, 115` · `registers.cpp:103` · `dwarf.cpp:979, 1099, 1238, 1561, 1707, 1715, 1732` · `target.cpp:300, 402, 418, 483` · `process.cpp:926` · `gsdb.cpp:352, 465, 549, 603, 760` — only `target.cpp:402` is stored beyond its scope (#7); the rest are local algorithm lambdas (CG F.52 allows `[&]` there).
- `.count(` (5): listed in #52.
- Mixed include styles: 18 files (see #49).
- `and`/`or`/`not` 257 vs `&&`/`||`/`!` 77 (see #35).

## Appendix B — Rules referenced (exact upstream titles)

CG A.1-A.4 (architecture) · C.9 Minimize exposure of members · C.12 Don't make data members `const` or references in a copyable or movable type · C.21 If you define or `=delete` any copy, move, or destructor function, define or `=delete` them all · C.31 All resources acquired by a class must be released by the class's destructor · C.32 If a class has a raw pointer (`T*`) or reference (`T&`), consider whether it might be owning · C.41 A constructor should create a fully initialized object · C.45 Don't define a default constructor that only initializes data members; use default member initializers instead · C.46 By default, declare single-argument constructors explicit · C.48 Prefer default member initializers to member initializers in constructors for constant initializers · C.49 Prefer initialization to assignment in constructors · C.50 Use a factory function if you need "virtual behavior" during initialization · C.82 Don't call virtual functions in constructors and destructors · C.86 Make `==` symmetric with respect to operand types and `noexcept` · C.90 Rely on constructors and assignment operators, not `memset` and `memcpy` · C.133 Avoid `protected` data · C.146 Use `dynamic_cast` where class hierarchy navigation is unavoidable · C.150 Use `make_unique()` to construct objects owned by `unique_ptr`s · C.153 Prefer virtual function to casting · C.161 Use non-member functions for symmetric operators · C.181 Avoid "naked" `union`s · C.182 Use anonymous `union`s to implement tagged unions · Con.2 By default, make member functions `const` · Con.4 Use `const` to define objects with values that do not change after construction · Con.5 Use `constexpr` for values that can be computed at compile time · CP.1 Assume that your code will run as part of a multi-threaded program · CP.200 Use `volatile` only to talk to non-C++ memory · CPL.3 If you must use C for interfaces, use C++ in the calling code using such interfaces · E.2 Throw an exception to signal that a function can't perform its assigned task · E.6 Use RAII to prevent leaks · E.14 Use purpose-designed user-defined types as exceptions (not built-in types) · E.15 Throw by value, catch exceptions from a hierarchy by reference · E.17 Don't try to catch every exception in every function · E.19 Use a `final_action` object to express cleanup if no suitable resource handle is available · E.27 If you can't throw exceptions, use error codes systematically · E.28 Avoid error handling based on global state (e.g. `errno`) · E.31 Properly order your catch-clauses · Enum.2 Use enumerations to represent sets of related named constants · Enum.3 Prefer class enums over "plain" enums · Enum.4 Define operations on enumerations for safe and simple use · Enum.6 Avoid unnamed enumerations · ES.1 Prefer the standard library to other libraries and to "handcrafted code" · ES.3 Don't repeat yourself, avoid redundant code · ES.10 Declare one name (only) per declaration · ES.12 Do not reuse names in nested scopes · ES.20 Always initialize an object · ES.22 Don't declare a variable until you have a value to initialize it with · ES.27 Use `std::array` or `stack_array` for arrays on the stack · ES.28 Use lambdas for complex initialization, especially of `const` variables · ES.31 Don't use macros for constants or "functions" · ES.32 Use `ALL_CAPS` for all macro names · ES.33 If you must use macros, give them unique names · ES.41 If in doubt about operator precedence, parenthesize · ES.45 Avoid "magic constants"; use symbolic constants · ES.46 Avoid lossy (narrowing, truncating) arithmetic conversions · ES.48 Avoid casts · ES.49 If you must use a cast, use a named cast · ES.50 Don't cast away `const` · ES.56 Write `std::move()` only when you need to explicitly move an object to another scope · ES.65 Don't dereference an invalid pointer · ES.71 Prefer a range-`for`-statement to a `for`-statement when there is a choice · ES.73 Prefer a `while`-statement to a `for`-statement when there is no obvious loop variable · ES.75 Avoid `do`-statements · ES.78 Don't rely on implicit fallthrough in `switch` statements · ES.87 Don't add redundant `==` or `!=` to conditions · ES.100 Don't mix signed and unsigned arithmetic · ES.101 Use unsigned types for bit manipulation · ES.102 Use signed types for arithmetic · ES.104 Don't underflow · ES.106 Don't try to avoid negative values by using `unsigned` · F.1 "Package" meaningful operations as carefully named functions · F.2 A function should perform a single logical operation · F.7 For general use, take `T*` or `T&` arguments rather than smart pointers · F.9 Unused parameters should be unnamed · F.16 For "in" parameters, pass cheaply-copied types by value and others by reference to `const` · F.18 For "will-move-from" parameters, pass by `X&&` and `std::move` the parameter · F.20 For "out" output values, prefer return values to output parameters · F.46 `int` is the return type for `main()` · F.49 Don't return `const T` · F.52 Prefer capturing by reference in lambdas that will be used locally, including passed to algorithms · F.53 Avoid capturing by reference in lambdas that will be used non-locally, including returned, stored on the heap, or passed to another thread · F.54 When writing a lambda that captures `this` or any class data member, don't use `[=]` default capture · F.60 Prefer `T*` over `T&` when "no argument" is a valid option · I.2 Avoid non-const global variables · I.4 Make interfaces precisely and strongly typed · I.5 State preconditions (if any) · I.6 Prefer `Expects()` for expressing preconditions · I.9 If an interface is a template, document its parameters using concepts · I.13 Do not pass an array as a single pointer · I.24 Avoid adjacent parameters that can be invoked by the same arguments in either order · NL.1 Don't say in comments what can be clearly stated in code · NL.2 State intent in comments · NL.3 Keep comments crisp · NL.8 Use a consistent naming style · NL.16 Use a conventional class member declaration order · NL.19 Avoid names that are easily misread · NL.25 Don't use `void` as an argument type · NL.27 Use a `.cpp` suffix for code files and `.h` for interface files · P.2 Write in ISO Standard C++ · P.4 Ideally, a program should be statically type safe · P.9 Don't waste time or space · P.11 Encapsulate messy constructs, rather than spreading through the code · P.12 Use supporting tools as appropriate · R.1 Manage resources automatically using resource handles and RAII · R.3 A raw pointer (a `T*`) is non-owning · R.10 Avoid `malloc()` and `free()` · R.12 Immediately give the result of an explicit resource allocation to a manager object · R.30 Take smart pointers as parameters only to explicitly express lifetime semantics · SF.1 Use a `.cpp` suffix for code files and `.h` for interface files if your project doesn't already follow another convention · SF.9 Avoid cyclic dependencies among source files · SF.10 Avoid dependencies on implicitly `#include`d names · SF.11 Header files should be self-contained · SF.12 Prefer the quoted form of `#include` for files relative to the including file and the angle bracket form everywhere else · SL.con.4 don't use `memset` or `memcpy` for arguments that are not trivially-copyable · SL.io.1 Use character-level input only when you have to · SL.io.2 When reading, always consider ill-formed input · SL.io.3 Prefer `iostream`s for I/O · SL.io.50 Avoid `endl` · SL.str.1 Use `std::string` to own character sequences · SL.str.2 Use `std::string_view` or `gsl::span<char>` to refer to character sequences · T.10 Specify concepts for all template arguments · T.11 Whenever possible use standard concepts · T.47 Avoid highly visible unconstrained templates with common names · T.144 Don't specialize function templates.

EMC++ #10, #15, #16, #17, #21, #31, #41 · EC++ #3, #4, #6, #9, #13, #28, #31, #53 · ESTL #4, #45 · CCS #0, #1, #2, #5, #13, #17, #18, #19, #22, #41, #48, #52, #53, #66, #90-#92 · ToTW #77, #94, #117, #134, #141, #142, #163, #188 · CERT ERR30-C, ERR50-CPP, ERR61-CPP, ERR62-CPP, FIO51-CPP, MEM50-CPP, MSC54-CPP, OOP50-CPP, SIG30-C, SIG31-C, STR37-C · HIC++ 7.1.4 · Google: Implicit Conversions, Function Names, Names and Order of Includes · Turner: warnings, `[[nodiscard]]`, clang-tidy, modern CMake · LLVM: early exits, no commented-out code.
