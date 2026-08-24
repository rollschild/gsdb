# Shared Library Loading & Tracing in gsdb — Complete Walkthrough

> Covers commits `f71df69` ("the rendezvous structure") and `6f21d12` ("shared libraries").
> All addresses, byte dumps, and outputs in this document were captured from live runs of
> `./build/tools/gsdb ./build/test/targets/marshmallow` on this machine with ASLR disabled
> (`personality(ADDR_NO_RANDOMIZE)` in `process::launch`), so they are reproducible.

---

## Table of Contents

1. [The problem: one ELF is not enough](#1-the-problem-one-elf-is-not-enough)
2. [Background: how a dynamic executable actually starts](#2-background-how-a-dynamic-executable-actually-starts)
3. [The four-step algorithm and where each step lives](#3-the-four-step-algorithm-and-where-each-step-lives)
4. [The data structures](#4-the-data-structures)
5. [Phase 1 — Bootstrap: `launch()` vs `attach()`](#5-phase-1--bootstrap-launch-vs-attach)
6. [Phase 2 — `resolve_dynamic_linker_rendezvous()`](#6-phase-2--resolve_dynamic_linker_rendezvous)
7. [Phase 3 — `reload_dynamic_libraries()`](#7-phase-3--reload_dynamic_libraries)
8. [Phase 4 — Incremental updates via `_dl_debug_state`](#8-phase-4--incremental-updates-via-_dl_debug_state)
9. [The hit-handler mechanism inside `wait_on_signal()`](#9-the-hit-handler-mechanism-inside-wait_on_signal)
10. [Address translation across many ELF objects](#10-address-translation-across-many-elf-objects)
11. [Downstream consumers: what became multi-ELF aware](#11-downstream-consumers-what-became-multi-elf-aware)
12. [End-to-end worked example with real numbers](#12-end-to-end-worked-example-with-real-numbers)
13. [Full interaction map (call graph)](#13-full-interaction-map-call-graph)
14. [Verified behavior](#14-verified-behavior)
15. [Gaps, deviations, and observations](#15-gaps-deviations-and-observations)
16. [Appendix — quick reference](#16-appendix--quick-reference)

---

## 1. The problem: one ELF is not enough

Before this work, `target` owned exactly one `std::unique_ptr<elf> elf_`. Every symbolic
operation — line lookup, function lookup, DWARF unwinding, PC→file-address conversion —
routed through that single object. That is fine for a statically linked program and
completely wrong for anything that links `libstdc++`.

```
                    BEFORE                                    AFTER
        ┌──────────────────────────┐              ┌────────────────────────────────┐
        │          target          │              │            target              │
        │                          │              │                                │
        │  unique_ptr<process>     │              │  unique_ptr<process>           │
        │  unique_ptr<elf>  elf_   │   ────►      │  elf_collection   elves_       │
        │  stack                   │              │  elf*             main_elf_    │
        │  stoppoint_collection    │              │  virt_addr  rendezvous_addr_   │
        │    <breakpoint>          │              │  stack                         │
        │                          │              │  stoppoint_collection          │
        └──────────────────────────┘              │    <breakpoint>                │
                                                  └────────────────────────────────┘
              1 ELF                                    N ELFs (main + all .so + vDSO)
```

Three consequences drive the entire design:

| Consequence | Where it shows up |
|---|---|
| A runtime address can belong to *any* of N objects, each with its **own load bias** | `virt_addr::to_file_addr(const elf_collection&)` |
| The set of objects is **not known at launch time** — it is discovered by asking the dynamic linker, and it can grow later via `dlopen` | `resolve_dynamic_linker_rendezvous()` / `reload_dynamic_libraries()` |
| A breakpoint the user typed may refer to code that **doesn't exist yet** | `breakpoint::resolve()` is re-run on every library load |

---

## 2. Background: how a dynamic executable actually starts

The `INTERP` program header names the dynamic linker. For `marshmallow`:

```
$ readelf -lW build/test/targets/marshmallow
Elf file type is DYN (Position-Independent Executable file)
Entry point 0x10a0

  Type           Offset   VirtAddr           FileSiz  MemSiz   Flg
  PHDR           0x000040 0x0000000000000040 0x0002d8 0x0002d8 R
  INTERP         0x000318 0x0000000000000318 0x000053 0x000053 R
      [Requesting program interpreter: /nix/store/…-glibc-2.42-51/lib/ld-linux-x86-64.so.2]
  LOAD           0x000000 0x0000000000000000 0x0009e8 0x0009e8 R
  LOAD           0x001000 0x0000000000001000 0x0002dd 0x0002dd R E
  LOAD           0x002000 0x0000000000002000 0x0001e8 0x0001e8 R
  LOAD           0x002d28 0x0000000000003d28 0x0002ec 0x0002f0 RW
  DYNAMIC        0x002d38 0x0000000000003d38 0x000250 0x000250 RW
```

Startup timeline:

```
 kernel space                                    user space
 ─────────────────────────────────────────────────────────────────────────────────────
 execve("marshmallow")
   │
   ├─ tear down old image (threads, maps, O_CLOEXEC fds, handlers)
   ├─ allocate stack
   ├─ map every LOAD segment of marshmallow      at bias B_exe
   ├─ map every LOAD segment of ld.so            at bias B_ld
   ├─ map the vDSO
   ├─ build auxv: AT_ENTRY = B_exe + e_entry, AT_BASE = B_ld, AT_SYSINFO_EHDR = vDSO
   └─ jump to  ld.so's  entry point ─────────────────┐
                                                     │
                                    ┌────────────────▼─────────────────────┐
                                    │ DYNAMIC LINKER (ld-linux-x86-64.so.2)│
                                    │                                      │
                                    │  read .dynamic of marshmallow        │
                                    │  for each DT_NEEDED: mmap the .so    │
                                    │  build the link_map linked list      │
                                    │  fill in DT_DEBUG  →  &r_debug       │
                                    │  perform relocations (GOT/PLT)       │
                                    │  run init functions                  │
                                    │                                      │
                                    │  jump to AT_ENTRY  ──────────────────┼──┐
                                    └──────────────────────────────────────┘  │
                                                                              │
                                    ┌─────────────────────────────────────────▼──┐
                                    │  marshmallow`_start  →  __libc_start_main  │
                                    │  →  main()                                 │
                                    └────────────────────────────────────────────┘
```

**The single most important fact for the debugger:** by the time control reaches
`AT_ENTRY`, the dynamic linker has *already* loaded every `DT_NEEDED` library and
populated the rendezvous structure. `AT_ENTRY` is therefore the perfect, free
synchronization point — and it is exactly what `target::launch` breakpoints.

### Why the `.dynamic` section must be read from *memory*, not from the file

```
$ readelf -d build/test/targets/marshmallow
 0x0000000000000015 (DEBUG)              0x0        ◄── zero on disk!
```

`DT_DEBUG`'s `d_ptr` is a **runtime-written slot**. The linker patches it with the address
of its own `r_debug` object. Reading the mmap'd file would yield `0x0`. This is why
`resolve_dynamic_linker_rendezvous()` calls `process_->read_memory(...)` rather than
`main_elf_->get_section_contents(".dynamic")`.

```
   elf's mmap of the file                 inferior's address space
   ┌──────────────────────┐               ┌──────────────────────────┐
   │ .dynamic             │               │ .dynamic (RW, RELRO)     │
   │  …                   │               │  …                       │
   │  DT_DEBUG   d_ptr=0  │  ✗ useless    │  DT_DEBUG  d_ptr=0x7ff…  │  ✓ what we want
   │  …                   │               │  …                       │
   └──────────────────────┘               └──────────────────────────┘
        get_section_contents()                  process::read_memory()
```

---

## 3. The four-step algorithm and where each step lives

`README.md` states the plan under *"Tracing Shared Library Loading"*. Here it is mapped
onto the implementation:

| # | Plan | Implementation | Location |
|---|---|---|---|
| 1 | Set an internal breakpoint on the real entry point; the linker is initialized when it hits | `target::launch` creates an internal `address_breakpoint` at `auxv[AT_ENTRY]` with a hit handler | `src/target.cpp:74-83` |
| 2 | Walk the loaded-library list in the rendezvous structure, parse each ELF, add to a collection in `target`; dump the vDSO to disk | `reload_dynamic_libraries()` + `dump_vdso()` | `src/target.cpp:416-465`, `src/target.cpp:46-63` |
| 3 | Set an internal breakpoint on `_dl_debug_state`, whose address is in `r_brk` | `resolve_dynamic_linker_rendezvous()` tail | `src/target.cpp:376-387` |
| 4 | On each `_dl_debug_state` hit with `r_state == RT_CONSISTENT`, reread `r_map`, adding new libs and unloading removed ones | Handler calls `reload_dynamic_libraries()` — **no `r_state` check, no unload path** (see [§15](#15-gaps-deviations-and-observations)) | `src/target.cpp:382-385` |

---

## 4. The data structures

### 4.1 Kernel/libc structs consumed (via `#include <link.h>` in `target.hpp:5`)

```c
/* the rendezvous structure — one per process, owned by ld.so */
struct r_debug {
    int r_version;              /*  +0  ==1 for this protocol            */
                                /*  +4  4 bytes of padding               */
    struct link_map *r_map;     /*  +8  head of the loaded-object list   */
    ElfW(Addr) r_brk;           /* +16  address of _dl_debug_state       */
    enum { RT_CONSISTENT,       /* +24  0 = list is stable                */
           RT_ADD,              /*      1 = an object is being added      */
           RT_DELETE } r_state; /*      2 = an object is being removed    */
                                /* +28  4 bytes of padding               */
    ElfW(Addr) r_ldbase;        /* +32  load bias of ld.so itself         */
};                              /* sizeof == 40                           */

/* one node per loaded object, doubly linked */
struct link_map {
    ElfW(Addr)       l_addr;    /*  +0  LOAD BIAS of this object          */
    char            *l_name;    /*  +8  absolute path, "" for main exe    */
    ElfW(Dyn)       *l_ld;      /* +16  this object's .dynamic (runtime)  */
    struct link_map *l_next;    /* +24                                    */
    struct link_map *l_prev;    /* +32                                    */
};                              /* sizeof == 40                           */

/* one .dynamic entry */
typedef struct {
    Elf64_Sxword d_tag;         /*  +0  DT_NEEDED / DT_DEBUG / …          */
    union { Elf64_Xword d_val;
            Elf64_Addr  d_ptr; } d_un;  /* +8                             */
} Elf64_Dyn;                    /* sizeof == 16                           */
```

`process::read_memory_as<T>()` (`process.hpp:182-186`) does the heavy lifting: it reads
`sizeof(T)` bytes out of the inferior and `memcpy`s them into a local `T` via
`from_bytes<T>`. Because these are POD structs with a fixed x86-64 layout, the debugger's
own copy of `struct r_debug` / `struct link_map` is a legitimate stand-in for the
inferior's. Pointer fields come back as *inferior* pointers, which is why the code
immediately launders them through `reinterpret_cast<std::uint64_t>` into `virt_addr`
rather than ever dereferencing them (`src/target.cpp:423`, `430`).

### 4.2 `elf_collection` — `include/libgsdb/elf.hpp:134-162`

A deliberately minimal owning container. No id space, no map, no ordering: three linear
searches and a `for_each`.

```
┌────────────────────────────── elf_collection ───────────────────────────────┐
│  std::vector<std::unique_ptr<elf>> elves_                                   │
│                                                                             │
│  push(unique_ptr<elf>)                        → append, take ownership      │
│  for_each(F)             / for_each(F) const  → visit every elf&            │
│  get_elf_containing_address(virt_addr) const  → O(N_elf × N_sections)       │
│  get_elf_by_path(path)                 const  → O(N_elf), exact path match  │
│  get_elf_by_filename(string_view)      const  → O(N_elf), basename match    │
└─────────────────────────────────────────────────────────────────────────────┘
```

Why three different lookups exist:

| Lookup | Used by | Why |
|---|---|---|
| `get_elf_containing_address` | `virt_addr::to_file_addr(const elf_collection&)` | the core "which object owns this PC?" query |
| `get_elf_by_path` | `reload_dynamic_libraries()` dedup for normal libraries | `l_name` is an absolute path, so it round-trips exactly |
| `get_elf_by_filename` | `reload_dynamic_libraries()` dedup for the **vDSO only** | the vDSO's `l_name` is the bare string `"linux-vdso.so.1"`, but the `elf` we built for it lives at `/tmp/gsdb-XXXXXX/linux-vdso.so.1`, so path equality can never match — basename equality can |

The `for_each` templates are defined in the header (`elf.hpp:151-162`) because they are
templates; both a mutable and a `const` overload exist so `const` methods like
`target::find_functions()` and `target::get_line_entries_by_line()` can iterate.

### 4.3 `target`'s new state — `include/libgsdb/target.hpp:120-129`

```cpp
std::unique_ptr<process> process_;
stack                    stack_;
stoppoint_collection<breakpoint> breakpoints_;
virt_addr                dynamic_linker_rendezvous_address_;  // &r_debug, or 0
elf_collection           elves_;                              // owns ALL ELF objects
elf*                     main_elf_;                           // non-owning alias
```

The private constructor (`target.hpp:106-109`) is subtle:

```cpp
target(std::unique_ptr<process> proc, std::unique_ptr<elf> obj)
    : process_(std::move(proc)), stack_(this), main_elf_(obj.get()) {
    elves_.push(std::move(obj));
}
```

`main_elf_` is captured from `obj.get()` in the *member-init list*, then the `unique_ptr`
is moved into `elves_` in the *body*. Moving a `unique_ptr` does not move the pointee, so
the raw alias stays valid — the main executable is simultaneously `elves_[0]` and
`*main_elf_`. `get_elf()` was retargeted from `*elf_` to `*main_elf_` (`target.hpp:48-49`)
so every existing caller kept working unchanged.

**Design note on `elf*` stability:** `elf_collection` stores `unique_ptr<elf>`, so
`push_back` reallocating the vector never moves the `elf` objects themselves. This matters
enormously, because `file_addr` stores a bare `const elf*` (`types.hpp:129`) and `dwarf`,
`compile_unit`, `die`, and `line_table` all hold pointers/`string_view`s into a specific
`elf`'s mmap. If the collection stored `elf` by value, every `dlopen` would dangle every
outstanding `file_addr` in the debugger. The indirection is load-bearing, not incidental.

### 4.4 `breakpoint`'s hit-handler hook — `include/libgsdb/breakpoint.hpp:68-81, 99`

```cpp
void install_hit_handler(std::function<bool(void)> on_hit) { on_hit_ = std::move(on_hit); }

bool notify_hit() const {
    if (on_hit_) return on_hit_();
    return false;          // no handler ⇒ "do not auto-restart"
}
```

The `bool` return is the entire protocol between library tracing and the process loop:

| Return | Meaning | Effect in `wait_on_signal()` |
|---|---|---|
| `true` | "this stop was mine, the user must not see it" | `resume()` then recurse into `wait_on_signal()` |
| `false` (incl. no handler) | "let this stop surface normally" | fall through, report to the CLI |

This is what makes the entry-point and `_dl_debug_state` breakpoints **invisible**. They
are also created with `internal = true`, which gives them `id_ == -1`
(`src/breakpoint.cpp:18`) so they never consume a user-visible breakpoint number and never
appear in `breakpoint list`.

---

## 5. Phase 1 — Bootstrap: `launch()` vs `attach()`

The two entry points reach the same state by different routes, because a freshly `exec`'d
process has *not* run the dynamic linker yet, whereas an already-running process has.

```
┌──────────────────────── target::launch (src/target.cpp:66-85) ────────────────────────┐
│                                                                                       │
│  process::launch(path, debug=true, stdout_replacement)                                │
│      └─ fork → PTRACE_TRACEME → execlp → SIGTRAP → wait_on_signal()                   │
│         ── inferior is halted at ld.so's entry, NOTHING loaded yet ──                  │
│                                                                                       │
│  create_loaded_elf(*proc, path)                                                       │
│      └─ new elf(path);  notify_loaded(auxv[AT_ENTRY] - e_entry)   ← bias of main exe   │
│                                                                                       │
│  new target(proc, obj)      → elves_ = [ marshmallow ],  main_elf_ = &marshmallow      │
│  proc.set_target(tgt)       → so wait_on_signal() can call target->notify_stop()       │
│                                                                                       │
│  entry_point = virt_addr{ auxv[AT_ENTRY] }                                            │
│  entry_bp    = create_address_breakpoint(entry_point, hardware=false, internal=true)   │
│  entry_bp.install_hit_handler([target] {                                              │
│                  target->resolve_dynamic_linker_rendezvous();                          │
│                  return true;              ← invisible: auto-resume                   │
│              });                                                                       │
│  entry_bp.enable()          → int3 patched at AT_ENTRY                                │
│                                                                                       │
│  return tgt   ── rendezvous NOT yet resolved; happens on the first resume ──           │
└───────────────────────────────────────────────────────────────────────────────────────┘

┌──────────────────────── target::attach (src/target.cpp:87-100) ────────────────────────┐
│                                                                                       │
│  elf_path = /proc/<pid>/exe          (symlink to the real binary)                      │
│  process::attach(pid)                (PTRACE_ATTACH → SIGSTOP → wait_on_signal)        │
│  create_loaded_elf(*proc, elf_path)                                                   │
│  new target(...) ; proc.set_target(tgt)                                               │
│                                                                                       │
│  tgt->resolve_dynamic_linker_rendezvous();   ← called DIRECTLY, no breakpoint needed   │
│                                              (linker already ran long ago)             │
└───────────────────────────────────────────────────────────────────────────────────────┘
```

Note the asymmetry consequence: after `target::launch` returns, `elves_` contains **only
the main executable**. A user who types `breakpoint set some_libc_function` at that instant
gets a breakpoint with **zero sites** — it resolves to nothing. It only acquires a site
when the first `continue` fires the entry-point breakpoint. This is observable and correct
behavior, demonstrated in [§14](#14-verified-behavior).

---

## 6. Phase 2 — `resolve_dynamic_linker_rendezvous()`

`src/target.cpp:346-390`. Runs once (idempotent via the guard on line 348).

```
┌─────────────────────────────────────────────────────────────────────────────────────┐
│ 1. GUARD                                                                            │
│    if (dynamic_linker_rendezvous_address_.addr()) return;                            │
│    ── a zero virt_addr doubles as "not yet resolved"                                 │
├─────────────────────────────────────────────────────────────────────────────────────┤
│ 2. LOCATE .dynamic IN THE FILE, THEN PROJECT IT INTO THE INFERIOR                    │
│    main_elf_->get_section(".dynamic")            → const Elf64_Shdr*                 │
│    dynamic_start = file_addr{*main_elf_, sh_addr}                                    │
│    dynamic_size  = sh_size                                                           │
│    read_memory(dynamic_start.to_virt_addr(), dynamic_size)                            │
│                          └─ file_addr → virt_addr adds elf::load_bias()               │
├─────────────────────────────────────────────────────────────────────────────────────┤
│ 3. REINTERPRET THE BYTES AS Elf64_Dyn[]                                              │
│    vector<Elf64_Dyn> entries(dynamic_size / sizeof(Elf64_Dyn));                       │
│    std::copy(bytes.begin(), bytes.end(), reinterpret_cast<std::byte*>(entries.data()))│
├─────────────────────────────────────────────────────────────────────────────────────┤
│ 4. SCAN FOR DT_DEBUG (== 21 == 0x15)                                                 │
│    for (auto entry : entries) if (entry.d_tag == DT_DEBUG) {                          │
│        dynamic_linker_rendezvous_address_ = virt_addr{entry.d_un.d_ptr};   ◄── &r_debug│
│        reload_dynamic_libraries();                       ← STEP 2 of the plan         │
│        auto debug_info = read_dynamic_linker_rendezvous();                            │
│        auto debug_state_addr = virt_addr{debug_info->r_brk};   ← &_dl_debug_state     │
│        auto& bp = create_address_breakpoint(debug_state_addr, false, /*internal=*/true)│
│        bp.install_hit_handler([&] { reload_dynamic_libraries(); return true; });       │
│        bp.enable();                                      ← STEP 3 of the plan         │
│    }                                                                                  │
└─────────────────────────────────────────────────────────────────────────────────────┘
```

### Verified live, on `marshmallow`

`.dynamic` sits at file vaddr `0x3d38`; the observed load bias is `0x0000555555554000`, so
the runtime address is `0x555555557d38`. `DT_DEBUG` is entry **index 18** (counting from
the `readelf -d` listing), i.e. `0x555555557d38 + 18*16 = 0x555555557e58`:

```
(gsdb) memory read 0x555555557e58 16
0x00555555557e58: 15 00 00 00 00 00 00 00   58 db ff f7 ff 7f 00 00
                  └── d_tag = 0x15 = 21 ──┘ └── d_ptr = 0x00007ffff7ffdb58 ─┘
                       == DT_DEBUG                  == &r_debug
```

Following that pointer:

```
(gsdb) memory read 0x7ffff7ffdb58 40
0x007ffff7ffdb58: 01 00 00 00 00 00 00 00   f0 e2 ff f7 ff 7f 00 00
0x007ffff7ffdb68: 60 78 fc f7 ff 7f 00 00   00 00 00 00 00 00 00 00
0x007ffff7ffdb78: 00 40 fc f7 ff 7f 00 00

  r_version = 1
  r_map     = 0x00007ffff7ffe2f0     ← head of the link_map list
  r_brk     = 0x00007ffff7fc7860     ← &_dl_debug_state
  r_state   = 0  (RT_CONSISTENT)
  r_ldbase  = 0x00007ffff7fc4000     ← ld.so's own load bias
```

And `r_brk` really is `_dl_debug_state`: `0x7ffff7fc7860 − r_ldbase 0x7ffff7fc4000 = 0x3860`.

```
$ readelf -sW …/ld-linux-x86-64.so.2 | grep 3860
    20: 0000000000003860     5 FUNC  GLOBAL DEFAULT  11 _dl_debug_state@@GLIBC_PRIVATE
   571: 0000000000003860     5 FUNC  LOCAL  DEFAULT  11 __GI__dl_debug_state
   651: 0000000000003860     5 FUNC  GLOBAL DEFAULT  11 _dl_debug_state
```

It is a **5-byte function whose entire purpose is to be breakpointed** — glibc calls it as
a no-op nop-hook so that debuggers have somewhere to trap.

---

## 7. Phase 3 — `reload_dynamic_libraries()`

`src/target.cpp:416-465`. This is the workhorse; it is called from Phase 2 once and from
the `_dl_debug_state` handler on every subsequent load.

```
                    ┌──────────────────────────────────────┐
                    │ debug = read_dynamic_linker_rendezvous│
                    │ if (!debug) return;                  │
                    └──────────────┬───────────────────────┘
                                   │
                    ┌──────────────▼───────────────────────┐
                    │ entry_ptr = debug->r_map             │
                    └──────────────┬───────────────────────┘
                                   │
                  ┌────────────────▼─────────────────┐
        ┌────────►│  while (entry_ptr != nullptr)    │──── nullptr ───► done
        │         └────────────────┬─────────────────┘
        │                          │
        │   ┌──────────────────────▼──────────────────────────────────────┐
        │   │ entry = read_memory_as<link_map>(virt_addr{entry_ptr})      │
        │   │ entry_ptr = entry.l_next            ◄── advance FIRST       │
        │   └──────────────────────┬──────────────────────────────────────┘
        │                          │
        │   ┌──────────────────────▼──────────────────────────────────────┐
        │   │ name_bytes = read_memory(virt_addr{entry.l_name}, 4096)     │
        │   │ name = path{ (char*)name_bytes.data() }   ← NUL-terminated  │
        │   │ if (name.empty()) continue;   ◄── skips the MAIN EXECUTABLE │
        │   └──────────────────────┬──────────────────────────────────────┘
        │                          │
        │   ┌──────────────────────▼──────────────────────────────────────┐
        │   │            name == "linux-vdso.so.1" ?                      │
        │   │   yes → found = elves_.get_elf_by_filename(name)            │
        │   │   no  → found = elves_.get_elf_by_path(name)                │
        │   └──────────────────────┬──────────────────────────────────────┘
        │                          │
        │              ┌───────────▼────────────┐
        │              │  found != nullptr ?    │
        │              └──┬──────────────────┬──┘
        │            yes  │                  │  no
        │      (already   │                  │
        │       tracked)  │   ┌──────────────▼─────────────────────────────┐
        │                 │   │  if vDSO:                                   │
        │                 │   │      name = dump_vdso(*process_,            │
        │                 │   │                       virt_addr{l_addr})    │
        │                 │   │  new_elf = make_unique<elf>(name)           │
        │                 │   │      └─ open + fstat + mmap + parse headers │
        │                 │   │         + symtab + build maps + new dwarf   │
        │                 │   │  new_elf->notify_loaded(virt_addr{l_addr})  │
        │                 │   │      └─ THE LOAD BIAS, straight from        │
        │                 │   │         link_map — no arithmetic needed     │
        │                 │   │  elves_.push(std::move(new_elf))            │
        │                 │   └──────────────┬─────────────────────────────┘
        │                 │                  │
        │              ┌──▼──────────────────▼──┐
        └──────────────┤ breakpoints_.for_each( │
                       │   bp.resolve() )        │  ← re-resolve EVERY breakpoint
                       └────────────────────────┘
```

### Three things worth pausing on

**(a) `l_addr` *is* the load bias.** For the main executable, `create_loaded_elf` has to
compute the bias as `auxv[AT_ENTRY] − e_entry` because that is the only cross-check the
kernel hands us. For shared objects, `link_map::l_addr` is literally the bias the linker
used, so `notify_loaded(virt_addr{entry.l_addr})` is a direct assignment. Verified: the
head `link_map` node reports `l_addr = 0x0000555555554000`, exactly matching the
`AT_ENTRY − e_entry` value computed for `marshmallow`.

**(b) The main executable is skipped by the empty-name test.** glibc sets `l_name = ""`
for the head node. Confirmed live:

```
(gsdb) memory read 0x7ffff7ffe2f0 40          ← link_map[0], the main executable
0x007ffff7ffe2f0: 00 40 55 55 55 55 00 00   c8 e8 ff f7 ff 7f 00 00
0x007ffff7ffe300: 38 7d 55 55 55 55 00 00   d0 e8 ff f7 ff 7f 00 00
0x007ffff7ffe310: 00 00 00 00 00 00 00 00

  l_addr = 0x0000555555554000   ← main exe load bias  ✔ matches AT_ENTRY − e_entry
  l_name = 0x00007ffff7ffe8c8
  l_ld   = 0x0000555555557d38   ← runtime .dynamic    ✔ matches 0x3d38 + bias
  l_next = 0x00007ffff7ffe8d0
  l_prev = 0x0000000000000000   ← head of list

(gsdb) memory read 0x7ffff7ffe8c8 8
0x007ffff7ffe8c8: 00 00 00 00 00 00 00 00   ← the empty string ⇒ `continue`
```

Without that `continue`, `get_elf_by_path("")` would miss, and the debugger would try
`new elf("")` and throw. The guard is doing real work, not defensive padding.

**(c) `bp.resolve()` runs *inside* the `while` loop, not after it.** So with 7 libraries
and 3 user breakpoints, `resolve()` is invoked 21 times rather than 3. It is idempotent —
every `resolve()` override guards with `if (!breakpoint_sites_.contains_address(...))` —
so this is wasted work, not a bug. See [§15](#15-gaps-deviations-and-observations).

### 7.1 The vDSO special case and `dump_vdso()`

The vDSO is a kernel-supplied ELF that exists **only in memory** — there is no file on
disk to `mmap`. But every downstream consumer (`elf`'s constructor, `dwarf`, `file_addr`)
is built around an mmap'd file. Rather than special-case the entire ELF layer,
`dump_vdso()` (`src/target.cpp:46-63`) materializes it:

```cpp
char tmp_dir[] = "/tmp/gsdb-XXXXXX";
mkdtemp(tmp_dir);                                        // unique dir per call
auto vdso_dump_path = std::filesystem::path(tmp_dir) / "linux-vdso.so.1";
std::ofstream vdso_dump(vdso_dump_path, std::ios::binary);

auto vdso_header = proc.read_memory_as<Elf64_Ehdr>(address);
auto vdso_size = vdso_header.e_shoff                     // start of section headers
               + vdso_header.e_shentsize * vdso_header.e_shnum;   // + their total size
auto vdso_bytes = proc.read_memory(address, vdso_size);
vdso_dump.write(reinterpret_cast<const char*>(vdso_bytes.data()), vdso_bytes.size());
return vdso_dump_path;
```

The size formula works because the section-header table is conventionally the **last**
thing in an ELF file, so `e_shoff + e_shentsize × e_shnum` is the total file length.

```
      in-memory vDSO image                        /tmp/gsdb-XXXXXX/linux-vdso.so.1
  ┌────────────────────────────┐                 ┌────────────────────────────┐
  │ Elf64_Ehdr                 │ ◄── l_addr      │ Elf64_Ehdr                 │
  ├────────────────────────────┤                 ├────────────────────────────┤
  │ program headers, .text,    │   read_memory   │ program headers, .text,    │
  │ .dynsym, .dynstr, …        │  ═════════════► │ .dynsym, .dynstr, …        │
  ├────────────────────────────┤   vdso_size     ├────────────────────────────┤
  │ section headers            │     bytes       │ section headers            │
  └────────────────────────────┘                 └────────────────────────────┘
    e_shoff ─┘  e_shnum × e_shentsize            then: new elf(that path)
                                                       notify_loaded(l_addr)
```

Verified — the dumped file is a well-formed ELF whose length matches the formula exactly:

```
$ ls -l /tmp/gsdb-1M9pin/linux-vdso.so.1
-rw-r--r-- 1 rollschild users 7176 …

$ readelf -h /tmp/gsdb-1M9pin/linux-vdso.so.1
  Type:                              DYN (Shared object file)
  Start of section headers:          6024 (bytes into file)
  Size of section headers:           64 (bytes)
  Number of section headers:         18
                                     ── 6024 + 64 × 18 = 7176  ✔
```

And this is exactly why the `get_elf_by_filename` branch exists: on the *second*
`reload_dynamic_libraries()` call, the link map still says `linux-vdso.so.1`, but the
tracked `elf`'s `path()` is `/tmp/gsdb-1M9pin/linux-vdso.so.1`. Basename comparison finds
it; path comparison would not, and the debugger would re-dump and re-`mmap` the vDSO on
every single library load.

---

## 8. Phase 4 — Incremental updates via `_dl_debug_state`

```
     inferior                                              gsdb
 ─────────────────────────────────────────────────────────────────────────────────
  main() calls dlopen("libmeow.so")
      │
      ├─ ld.so mmaps libmeow.so
      ├─ ld.so links the new link_map node into r_map
      ├─ r_debug.r_state = RT_ADD
      ├─ call _dl_debug_state()  ──► int3 ──► SIGTRAP ──►  waitpid returns
      │                                                     augment_stop_reason
      │                                                       → software_break
      │                                                     PC -= 1
      │                                                     site.parent_->notify_hit()
      │                                                       → reload_dynamic_libraries()
      │                                                          • walk r_map
      │                                                          • libmeow.so not found
      │                                                          • new elf(...) + bias
      │                                                          • elves_.push(...)
      │                                                          • breakpoints_.for_each(
      │                                                              resolve)  ← NEW SITE
      │                                                       → return true
      │                                                     resume(); wait_on_signal()
      ◄──────────────────────────────────────────────────────  (recursion)
      ├─ ld.so performs relocations, runs init
      ├─ r_debug.r_state = RT_CONSISTENT
      ├─ call _dl_debug_state()  ──► int3 ──► SIGTRAP ──►  same path again
      │                                                     (idempotent: nothing new)
      ◄──────────────────────────────────────────────────────  resume
      │
  dlopen returns; main() calls the new function
      └─ hits the site gsdb just planted  ──► SIGTRAP ──►  no parent handler that
                                                            returns true ⇒ SURFACES
                                                            to the user
```

The two `_dl_debug_state` calls per `dlopen` (one `RT_ADD`, one `RT_CONSISTENT`) both run
the handler, because the implementation does not inspect `r_state`. Since the handler only
ever *adds*, and adding is guarded by a path lookup, the duplicate call is harmless.

### Verified live

Note that this path is **not covered by the test suite** — no target uses `dlopen`. I
verified it manually with a scratch program in `/tmp` (not added to the repo):

```cpp
int libmeow_client_cuteness = 100;          // libmeow.so needs this from the exe
int main() {
    void* h = dlopen(".../libmeow.so", RTLD_NOW);
    auto f = (bool(*)())dlsym(h, "_Z22libmeow_client_is_cutev");
    printf("is_cute=%d\n", f());
}
```

```
$ printf 'breakpoint set libmeow_client_is_cute\nbreakpoint list\ncontinue\nbreakpoint list\nbacktrace\n' \
    | ./build/tools/gsdb /tmp/gsdb-dlopen-check/lateload

Launched process with PID 126476
Current breakpoints:
1: function = libmeow_client_is_cute, enabled:      ◄── ZERO sites: lib not loaded yet
Process 126476 stopped with signal TRAP at 0x7ffff7e870fd, libmeow.cpp:3
                                    (libmeow.so`libmeow_client_is_cute) (breakpoint 1)
  3 bool libmeow_client_is_cute() { return libmeow_client_cuteness > 50; }
Current breakpoints:
1: function = libmeow_client_is_cute, enabled:
    .1: address = 0x7ffff7e870fd, enabled           ◄── site materialized at dlopen time
*[0]: 0x7ffff7e870fd libmeow.so`libmeow_client_is_cute
 [1]: 0x5555555551f6 lateload`main
```

A breakpoint on a function in a library that did not exist when the user typed the command
resolved and fired correctly, with a backtrace that crosses the ELF boundary.

---

## 9. The hit-handler mechanism inside `wait_on_signal()`

`src/process.cpp:196-212`. This is the one change in `process` that the whole feature rests
on, and it is worth reading closely.

```cpp
auto instr_begin = get_pc() - 1;
if (reason.info == SIGTRAP) {
    if (reason.trap_reason == trap_type::software_break and
        breakpoint_sites_.contains_address(instr_begin) and          // (A)
        breakpoint_sites_.get_by_address(instr_begin).is_enabled()) {
        set_pc(instr_begin);                                         // (B)

        auto& bp = breakpoint_sites_.get_by_address(instr_begin);
        if (bp.parent_) {                                            // (C)
            bool should_restart = bp.parent_->notify_hit();
            if (should_restart) {
                resume();
                return wait_on_signal();                             // (D)
            }
        }
    } else if (…hardware_break…) { … }
      else if (…syscall…)        { … }
}
if (target_) target_->notify_stop(reason);                           // (E)
```

**(A)** The predicate was rewritten from `enabled_stoppoint_at_address(instr_begin) and
get_by_address(instr_begin).is_enabled()` to `contains_address(instr_begin) and
get_by_address(instr_begin).is_enabled()`. The old form was redundant —
`enabled_stoppoint_at_address` already *is* `contains_address(a) and
get_by_address(a).is_enabled()` (`stoppoint_collection.hpp:141-145`), so the original
checked `is_enabled()` twice while performing three linear scans. The new form is
semantically identical with two scans.

**(B)** The PC rewind must happen *before* the handler runs, because
`reload_dynamic_libraries()` → `bp.resolve()` may plant new `int3` bytes, and any later
code that reads the PC (including the recursive `wait_on_signal`) needs the corrected
value.

**(C)** `breakpoint_site::parent_` (`breakpoint_site.hpp:45`) is the back-pointer from the
low-level site to the high-level `breakpoint` that owns it. It is `nullptr` for sites
created through the two-argument `create_breakpoint_site(address, hardware, internal)`
overload — which is what `target::run_until_address()` uses for its temporary stepping
breakpoints. Those correctly bypass the handler machinery entirely.

**(D)** Tail recursion, not a loop. Each auto-restarted internal stop consumes one stack
frame of `wait_on_signal`. In practice the depth is tiny (2 frames per `dlopen`), but a
program that loads hundreds of plugins in a tight loop would nest hundreds deep. A `while`
loop would be strictly better; see [§15](#15-gaps-deviations-and-observations).

**(E)** Because the internal path returns early at **(D)**, `target_->notify_stop()` —
and therefore `stack::unwind()` — is **not** run for stops the user never sees. The
unwinder only ever runs on stops that surface. That is both a correctness requirement
(unwinding at `_dl_debug_state`, mid-`RT_ADD`, would produce garbage) and a nice
performance property.

### Control-flow summary

```
                        SIGTRAP arrives in wait_on_signal
                                    │
                     augment_stop_reason (PTRACE_GETSIGINFO)
                                    │
                    ┌───────────────┴────────────────┐
                    │  si_code == SI_KERNEL          │
                    │  ⇒ trap_type::software_break   │
                    └───────────────┬────────────────┘
                                    │
                 site exists at PC-1 and is enabled?
                       ┌────────────┴────────────┐
                    no │                         │ yes
                       │                  set_pc(PC-1)
                       │                         │
                       │                site->parent_ != nullptr?
                       │              ┌──────────┴──────────┐
                       │           no │                     │ yes
                       │              │            parent_->notify_hit()
                       │              │                     │
                       │              │        ┌────────────┴────────────┐
                       │              │  false │                         │ true
                       │              │        │                  resume()
                       │              │        │                  return wait_on_signal()
                       │              │        │                         ▲
                       ▼              ▼        ▼                         │
                  ┌────────────────────────────────────┐            (recursion —
                  │ target_->notify_stop(reason)       │             user never
                  │   └─ stack_.unwind()               │             sees this stop)
                  │ return reason  →  CLI reports it   │
                  └────────────────────────────────────┘
```

---

## 10. Address translation across many ELF objects

This is the conceptual heart of the change. `file_addr` carries a `const elf*`
(`types.hpp:129`) precisely so that "which object?" is never ambiguous — but somebody has
to answer that question when starting from a raw runtime PC.

```
  virt_addr  (what ptrace gives you: 0x7ffff7fb60fd)
      │
      │  to_file_addr(const elf_collection&)      src/types.cpp:26-31    ← NEW
      │      └─ elves.get_elf_containing_address(*this)   src/elf.cpp:274-283
      │             └─ for each elf: elf->get_section_containing_address(virt_addr)
      │                                    src/elf.cpp:140-150
      │                       └─ addr >= bias + sh_addr && addr < bias + sh_addr + sh_size
      │      └─ if none → file_addr{}  (null elf_, addr 0)
      │      └─ else → to_file_addr(*obj)
      │
      ▼
  file_addr  { elf_ = &libmeow_elf, addr_ = 0x10fd }
      │
      │  to_virt_addr()                            src/types.cpp
      │      └─ obj.load_bias() + addr_
      ▼
  virt_addr  (round trip)
```

The two overloads now coexist:

| Overload | Semantics | Use when |
|---|---|---|
| `to_file_addr(const elf& obj)` | "interpret this address *as belonging to* `obj`"; returns `file_addr{}` if the address isn't in any of `obj`'s sections | you already know the object (e.g. `elf::get_symbol_at_address(virt_addr)`) |
| `to_file_addr(const elf_collection& elves)` | "**find** the object that owns this address, then convert" | you have a raw PC and no idea where it came from |

Three call sites were switched from the single-ELF form to the collection form — and those
three switches are what actually make shared-library debugging work:

```diff
  // src/target.cpp:102-104
- return process_->get_pc().to_file_addr(*elf_);
+ return process_->get_pc().to_file_addr(elves_);

  // src/target.cpp:318  (function_name_at_address)
- auto file_address = address.to_file_addr(*elf_);
+ auto file_address = address.to_file_addr(elves_);

  // src/stack.cpp:104   (inside the unwind loop)
- file_pc = virt_pc.to_file_addr(target_->get_elf());
+ file_pc = virt_pc.to_file_addr(target_->get_elves());
```

`get_pc_file_address()` is the single most consequential one, because it feeds
`line_entry_at_pc()`, `stack::inline_stack_at_pc()`, `stack::unwind()`, `step_in()`,
`step_over()`, and `step_out()`. Making that one function collection-aware retro-fitted
shared-library support onto the entire source-level stepping layer for free — no changes
to `step_in`/`step_over`/`step_out` were needed at all.

### The unwinder boundary condition

`src/stack.cpp:79`:

```diff
- while (virt_pc.addr() != 0 and elf == &target_->get_elf()) {
+ while (virt_pc.addr() != 0 and elf) {
```

The old condition literally said "stop unwinding the moment the return address leaves the
main executable" — the original comment even acknowledged this was the intended meaning
("indicating that this function belongs to some shared library or that we've hit the
topmost frame"). With a collection, `elf` being non-null *is* the correct termination
test: `to_file_addr(elves_)` returns a null-`elf` `file_addr` exactly when the PC belongs
to no tracked object, which is the real "we've run off the end" signal.

```
                BEFORE                                    AFTER
    ┌─────────────────────────────┐          ┌─────────────────────────────┐
    │ [0] libmeow_client_is_cute  │          │ [0] libmeow_client_is_cute  │
    └─────────────────────────────┘          ├─────────────────────────────┤
      unwind stops: elf != main_elf          │ [1] main                    │
      backtrace has 1 frame                  ├─────────────────────────────┤
                                             │ … __libc_start_main, _start │
                                             └─────────────────────────────┘
                                               unwind stops when CFI runs out
```

---

## 11. Downstream consumers: what became multi-ELF aware

Five functions had to learn about `elves_`. Note the shape they all share: an
`elves_.for_each` that accumulates across objects, replacing a single-object query.

### 11.1 `target::find_functions` — `src/target.cpp:276-294`

```diff
- auto dwarf_found = elf_->get_dwarf().find_functions(name);
- if (dwarf_found.empty()) {
-     auto elf_found = elf_->get_symbols_by_name(name);
-     for (auto sym : elf_found) res.elf_functions.push_back({elf_.get(), sym});
- } else {
-     res.dwarf_functions.insert(end, dwarf_found.begin(), dwarf_found.end());
- }
+ elves_.for_each([&](auto& elf) {
+     auto dwarf_found = elf.get_dwarf().find_functions(name);
+     if (dwarf_found.empty()) {
+         auto elf_found = elf.get_symbols_by_name(name);
+         for (auto sym : elf_found) res.elf_functions.push_back({&elf, sym});
+     } else {
+         res.dwarf_functions.insert(end, dwarf_found.begin(), dwarf_found.end());
+     }
+ });
```

The DWARF-preferred-over-symtab fallback is now **per object**, which is the right
granularity: `libmeow.so` is built with `-g -gdwarf-4` so it contributes a DWARF DIE, while
a stripped `libc.so.6` in the same process contributes `.dynsym` entries. Both land in the
same `find_functions_result`, and `function_breakpoint::resolve()` (`src/breakpoint.cpp:41-86`)
already handles both vectors — it plants prologue-skipped sites for DWARF hits and raw
`st_value` sites for ELF-symbol hits.

Note the `std::pair{&elf, sym}` in `elf_functions`: that `const elf*` is what
`function_breakpoint::resolve()` uses at `src/breakpoint.cpp:76` to build
`file_addr{*sym.first, sym.second->st_value}` — i.e. the symbol's file address is tagged
with *its own* object so `to_virt_addr()` applies *that library's* bias. Without carrying
the `elf*` through the result, every symbol would be biased by the main executable's load
address.

### 11.2 `target::get_line_entries_by_line` — `src/target.cpp:395-406` (new)

```cpp
std::vector<gsdb::line_table::iterator> gsdb::target::get_line_entries_by_line(
    std::filesystem::path path, std::size_t line) const {
    std::vector<gsdb::line_table::iterator> entries;
    elves_.for_each([&](auto& elf) {
        for (auto& cu : elf.get_dwarf().compile_units()) {
            auto new_entries = cu->lines().get_entries_by_line(path, line);
            entries.insert(entries.end(), new_entries.begin(), new_entries.end());
        }
    });
    return entries;
}
```

This is a pure hoist of the loop that used to live inside `line_breakpoint::resolve()`,
widened by one level (`elves_ × compile_units` instead of just `compile_units`). Putting
it on `target` is the right home: `target` is the only thing that owns the collection, and
now any future consumer of "all line entries for file:line" gets multi-ELF behavior for
free.

### 11.3 `line_breakpoint::resolve` — `src/breakpoint.cpp:93-125`

The change is almost entirely **de-nesting**:

```diff
- auto& dwarf = target_->get_elf().get_dwarf();
- for (auto& cu : dwarf.compile_units()) {
-     auto entries = cu->lines().get_entries_by_line(file_, line_);
-     for (auto entry : entries) {
-         auto& dwarf = entry->address.elf_file()->get_dwarf();
-         …
-     }
- }
+ auto entries = target_->get_line_entries_by_line(file_, line_);
+ for (auto entry : entries) {
+     auto& dwarf = entry->address.elf_file()->get_dwarf();
+     …
+ }
```

The body was already correct for shared libraries — the existing comment says it outright:

> *"Grab the DWARF file from the line table entry rather than using the one we got from the
> target. This is to support shared libs"*

`entry->address` is a `file_addr`, which carries its own `const elf*`, so
`entry->address.elf_file()->get_dwarf()` reaches the *right* DWARF for the inline-stack
and prologue-skip logic, and `entry->address.to_virt_addr()` applies the *right* bias. The
work in this commit was to feed that already-correct body from all objects instead of one.

### 11.4 `target::function_name_at_address` — `src/target.cpp:316-344`

Now qualifies every name with the owning object:

```diff
- auto file_address = address.to_file_addr(*elf_);
+ auto file_address = address.to_file_addr(elves_);
  auto obj = file_address.elf_file();
  if (!obj) return "";

  auto func = obj->get_dwarf().function_containing_address(file_address);
+ auto elf_filename = obj->path().filename().string();
+ std::string func_name = "";
  if (func and func->name()) {
-     return std::string(*func->name());
+     func_name = *func->name();
  } else if (auto elf_func = obj->get_symbol_containing_address(file_address);
             elf_func and ELF64_ST_TYPE(elf_func.value()->st_info) == STT_FUNC) {
-     return abi::__cxa_demangle(elf_name.c_str(), nullptr, nullptr, nullptr);
+     func_name = obj->get_string(elf_func.value()->st_name);
  }
+ if (!func_name.empty()) return elf_filename + "`" + func_name;
  return "";
```

The output format is `<basename>`<function>` — LLDB's convention:

```
*[0]: 0x7ffff7fb60fd libmeow.so`libmeow_client_is_cute
 [1]: 0x5555555551fc marshmallow`main
```

This is why the `[target]` stepping test assertions had to be updated from `"main"` to
`"step`main"` (`test/tests.cpp:826, 834, 840, 856, 861`).

Two regressions hitched a ride here, both flagged in [§15](#15-gaps-deviations-and-observations):
the `__cxa_demangle` call was commented out (so ELF-symbol names now surface **mangled**),
and `elf_name` at `src/target.cpp:333` is now a dead local.

### 11.5 `target`'s new public accessors — `include/libgsdb/target.hpp:97-103`

```cpp
elf_collection&       get_elves()          { return elves_; }
const elf_collection& get_elves()    const { return elves_; }
elf&                  get_main_elf()       { return *main_elf_; }
const elf&            get_main_elf() const { return *main_elf_; }
```

`get_elves()` is consumed by `stack::unwind()` (`src/stack.cpp:104`). `get_main_elf()` is
currently a synonym for the pre-existing `get_elf()`, added for clarity at call sites that
specifically mean "the executable, not some library". The CLI (`tools/gsdb.cpp`) does not
reference either — it observes shared libraries purely through `function_name_at_address`
and the stack, which is a good sign the abstraction landed in the right place.

---

## 12. End-to-end worked example with real numbers

Target: `marshmallow` (executable) + `libmeow.so` (shared library).

```cpp
// test/targets/marshmallow.cpp                  // test/targets/libmeow.cpp
int libmeow_client_cuteness = 100;               extern int libmeow_client_cuteness;
bool libmeow_client_is_cute();
                                                 bool libmeow_client_is_cute() {
int main() {                                         return libmeow_client_cuteness > 50;
    std::cout << libmeow_client_cuteness;        }
    std::cout << libmeow_client_is_cute();
}
```

The dependency is deliberately **circular across the boundary**: the executable calls into
the library, and the library reads a variable defined in the executable. That exercises
both relocation directions.

```
$ readelf -rW build/test/targets/libmeow.so
Relocation section '.rela.dyn':
0000000000003fe0  R_X86_64_GLOB_DAT   libmeow_client_cuteness + 0     ← lib → exe (data)

$ readelf -rW build/test/targets/marshmallow
Relocation section '.rela.plt':
0000000000003fa8  R_X86_64_JUMP_SLOT  _Z22libmeow_client_is_cutev + 0 ← exe → lib (code)

$ readelf -d build/test/targets/marshmallow | grep FLAGS
 0x000000000000001e (FLAGS)   BIND_NOW
 0x000000006ffffffb (FLAGS_1) Flags: NOW PIE
```

Worth noting: this toolchain sets `BIND_NOW`, so the PLT slot is resolved eagerly at
startup rather than lazily. The PLT still exists as an indirection, but no lazy-binding
trampoline runs. gsdb's tracing is unaffected either way — it keys off the link map, not
off PLT state.

### Symbol geometry in `libmeow.so`

```
$ readelf -sW build/test/targets/libmeow.so | grep is_cute
     6: 00000000000010f9    21 FUNC  GLOBAL DEFAULT  11 _Z22libmeow_client_is_cutev
        └─ file addr 0x10f9, 21 bytes, in section 11 (.text @ 0x1040)
```

### The full timeline with observed values

```
 ┌──────┬──────────────────────────────────────┬─────────────────────────────────────┐
 │ STEP │ ACTION                               │ OBSERVED STATE                      │
 ├──────┼──────────────────────────────────────┼─────────────────────────────────────┤
 │  1   │ target::launch("marshmallow")        │ elves_ = [marshmallow]              │
 │      │  bias = AT_ENTRY − e_entry           │ bias   = 0x0000555555554000         │
 │      │  entry bp @ AT_ENTRY                 │        = 0x5555555550a0 (int3)      │
 ├──────┼──────────────────────────────────────┼─────────────────────────────────────┤
 │  2   │ user: breakpoint set                 │ breakpoint 1, ZERO sites            │
 │      │       libmeow_client_is_cute         │ (libmeow.so unknown so far)         │
 ├──────┼──────────────────────────────────────┼─────────────────────────────────────┤
 │  3   │ user: continue                       │ ld.so loads all DT_NEEDED, then     │
 │      │                                      │ jumps to AT_ENTRY → int3 → SIGTRAP  │
 ├──────┼──────────────────────────────────────┼─────────────────────────────────────┤
 │  4   │ wait_on_signal: software_break,      │ notify_hit() →                      │
 │      │  PC -= 1, parent_->notify_hit()      │  resolve_dynamic_linker_rendezvous  │
 ├──────┼──────────────────────────────────────┼─────────────────────────────────────┤
 │  5   │ read .dynamic from MEMORY            │ @0x555555557d38, 0x250 bytes        │
 │      │ scan for DT_DEBUG (entry #18)        │ @0x555555557e58: tag 0x15           │
 │      │                                      │ d_ptr = 0x00007ffff7ffdb58          │
 ├──────┼──────────────────────────────────────┼─────────────────────────────────────┤
 │  6   │ read_memory_as<r_debug>              │ r_version = 1                       │
 │      │                                      │ r_map     = 0x00007ffff7ffe2f0      │
 │      │                                      │ r_brk     = 0x00007ffff7fc7860      │
 │      │                                      │ r_state   = 0 (RT_CONSISTENT)       │
 │      │                                      │ r_ldbase  = 0x00007ffff7fc4000      │
 ├──────┼──────────────────────────────────────┼─────────────────────────────────────┤
 │  7   │ reload_dynamic_libraries: walk r_map │ see the link-map table below        │
 ├──────┼──────────────────────────────────────┼─────────────────────────────────────┤
 │  8   │ breakpoints_.for_each(resolve)       │ find_functions now sees libmeow's   │
 │      │  function_breakpoint::resolve        │ DWARF → low_pc 0x10f9 → line entry  │
 │      │  → prologue skip (++function_line)   │ → 0x10fd  (skips push %rbp;         │
 │      │                                      │            mov %rsp,%rbp = 4 bytes) │
 │      │  → to_virt_addr(): bias + 0x10fd     │ = 0x7ffff7fb5000 + 0x10fd           │
 │      │                                      │ = 0x7ffff7fb60fd   ← int3 planted   │
 ├──────┼──────────────────────────────────────┼─────────────────────────────────────┤
 │  9   │ create bp on r_brk (_dl_debug_state) │ @0x7ffff7fc7860, internal, id −1    │
 ├──────┼──────────────────────────────────────┼─────────────────────────────────────┤
 │ 10   │ handler returns TRUE                 │ resume(); wait_on_signal() recurse  │
 │      │                                      │ ── user never saw step 4–9 ──       │
 ├──────┼──────────────────────────────────────┼─────────────────────────────────────┤
 │ 11   │ marshmallow`main calls into libmeow  │ int3 @0x7ffff7fb60fd → SIGTRAP      │
 │      │  parent_->notify_hit() → no handler  │ returns false ⇒ SURFACES            │
 ├──────┼──────────────────────────────────────┼─────────────────────────────────────┤
 │ 12   │ target_->notify_stop → stack_.unwind │ frame[0] libmeow.so + CFI unwind    │
 │      │  loop no longer stops at ELF edge    │ frame[1] marshmallow`main           │
 └──────┴──────────────────────────────────────┴─────────────────────────────────────┘
```

### The observed link map

Walked node-by-node with `memory read`, 40 bytes at a time:

| # | node address | `l_addr` (bias) | `l_name` → string | `l_prev` | `l_next` | gsdb action |
|---|---|---|---|---|---|---|
| 0 | `0x7ffff7ffe2f0` | `0x555555554000` | `0x7ffff7ffe8c8` → `""` | `0x0` | `0x7ffff7ffe8d0` | **skipped** (empty name) — already `elves_[0]` |
| 1 | `0x7ffff7ffe8d0` | `0x7ffff7fc2000` | `0x7ffff7fc2432` → `"linux-vdso.so.1"` | `0x7ffff7ffe2f0` | `0x7ffff7fba440` | `dump_vdso` → `/tmp/gsdb-XXXXXX/linux-vdso.so.1`, then `new elf` |
| 2 | `0x7ffff7fba440` | `0x7ffff7fb5000` | `0x7ffff7fba400` → `".../build/test/targets/libmeow.so"` | `0x7ffff7ffe8d0` | `0x7ffff7fba9c0` | `new elf` + `notify_loaded(0x7ffff7fb5000)` |
| 3… | … | … | `libstdc++.so.6`, `libm.so.6`, `libgcc_s.so.1`, `libc.so.6`, `ld-linux-x86-64.so.2` | | | `new elf` each |

Raw bytes for node 1 and its name string, as proof of the vDSO path:

```
(gsdb) memory read 0x7ffff7ffe8d0 40
0x007ffff7ffe8d0: 00 20 fc f7 ff 7f 00 00   32 24 fc f7 ff 7f 00 00
0x007ffff7ffe8e0: a8 24 fc f7 ff 7f 00 00   40 a4 fb f7 ff 7f 00 00
0x007ffff7ffe8f0: f0 e2 ff f7 ff 7f 00 00
     l_addr=0x7ffff7fc2000  l_name=0x7ffff7fc2432  l_ld=0x7ffff7fc24a8
     l_next=0x7ffff7fba440  l_prev=0x7ffff7ffe2f0

(gsdb) memory read 0x7ffff7fc2432 16
0x007ffff7fc2432: 6c 69 6e 75 78 2d 76 64 73 6f 2e 73 6f 2e 31 00
                  l  i  n  u  x  -  v  d  s  o  .  s  o  .  1  \0
```

Note `l_name` = `0x7ffff7fc2432` = `l_addr + 0x432` — the vDSO's name string lives *inside
the vDSO image itself*, which is why the dumped copy is self-consistent.

### Final address arithmetic, closing the loop

```
   file addr of _Z22libmeow_client_is_cutev          0x00000000000010f9
 + prologue (push %rbp = 1, mov %rsp,%rbp = 3)     + 0x0000000000000004
   ────────────────────────────────────────────────────────────────────
   file addr of first body instruction               0x00000000000010fd
 + libmeow.so load bias (from link_map.l_addr)     + 0x00007ffff7fb5000
   ────────────────────────────────────────────────────────────────────
   runtime breakpoint address                        0x00007ffff7fb60fd   ✔
```

And the debugger reports exactly that:

```
$ printf 'breakpoint set libmeow_client_is_cute\nbreakpoint list\ncontinue\nbacktrace\n' \
    | ./build/tools/gsdb ./build/test/targets/marshmallow

Launched process with PID 124558
Current breakpoints:
1: function = libmeow_client_is_cute, enabled:
Process 124558 stopped with signal TRAP at 0x7ffff7fb60fd, libmeow.cpp:3
                                    (libmeow.so`libmeow_client_is_cute) (breakpoint 1)
  1 extern int libmeow_client_cuteness;
  2
> 3 bool libmeow_client_is_cute() { return libmeow_client_cuteness > 50; }
  4
*[0]: 0x7ffff7fb60fd libmeow.so`libmeow_client_is_cute
 [1]: 0x5555555551fc marshmallow`main
```

Everything is present: the correct library, the correct source file and line from
`libmeow.so`'s own DWARF, the object-qualified function name, and a backtrace that walks
out of the library into the executable.

---

## 13. Full interaction map (call graph)

### 13.1 Who calls whom

```
                        ┌───────────────────────┐
                        │   tools/gsdb.cpp      │
                        │   (REPL)              │
                        └──────────┬────────────┘
                                   │ target::launch / attach
                                   │ create_*_breakpoint
                                   │ function_name_at_address
                                   │ get_stack().frames()
                                   ▼
 ┌───────────────────────────────────────────────────────────────────────────────┐
 │                                  target                                       │
 │                                                                               │
 │  launch() ──────────► create_address_breakpoint(AT_ENTRY, internal)           │
 │      │                        └─► install_hit_handler(λ → resolve_rendezvous) │
 │      │                                                                       │
 │  attach() ──────────► resolve_dynamic_linker_rendezvous()  (direct)           │
 │                                    │                                          │
 │  resolve_dynamic_linker_rendezvous()                                          │
 │      ├─► main_elf_->get_section(".dynamic")                                   │
 │      ├─► file_addr::to_virt_addr()                                            │
 │      ├─► process_->read_memory()                    ── scan DT_DEBUG          │
 │      ├─► reload_dynamic_libraries()                                           │
 │      ├─► read_dynamic_linker_rendezvous()                                     │
 │      └─► create_address_breakpoint(r_brk, internal)                           │
 │                 └─► install_hit_handler(λ → reload_dynamic_libraries)         │
 │                                                                               │
 │  read_dynamic_linker_rendezvous()                                             │
 │      └─► process_->read_memory_as<r_debug>()                                  │
 │                                                                               │
 │  reload_dynamic_libraries()                                                   │
 │      ├─► process_->read_memory_as<link_map>()        (per node)               │
 │      ├─► process_->read_memory()                     (per l_name, 4096 B)     │
 │      ├─► elves_.get_elf_by_path / get_elf_by_filename                         │
 │      ├─► dump_vdso(*process_, l_addr)          [vDSO only]                     │
 │      ├─► new elf(path) ─► elf::notify_loaded(l_addr)                          │
 │      ├─► elves_.push()                                                        │
 │      └─► breakpoints_.for_each(bp.resolve())                                  │
 │                                                                               │
 │  get_pc_file_address() ──► virt_addr::to_file_addr(elves_)                     │
 │  find_functions()      ──► elves_.for_each(...)                               │
 │  get_line_entries_by_line() ──► elves_.for_each(cu->lines()...)               │
 │  function_name_at_address() ──► to_file_addr(elves_) + obj->path().filename()  │
 └───────────────────────────────────────────────────────────────────────────────┘
        │                          │                        │
        │ owns                     │ owns                   │ owns
        ▼                          ▼                        ▼
 ┌──────────────┐   ┌──────────────────────────┐  ┌─────────────────────────────┐
 │   process    │   │      elf_collection      │  │ stoppoint_collection        │
 │              │   │  vector<unique_ptr<elf>> │  │   <breakpoint>              │
 │ wait_on_     │   │                          │  │                             │
 │  signal()    │   │  elves_[0] = main exe    │  │  address_breakpoint   ┐      │
 │   └─ site.   │   │  elves_[1] = vDSO dump   │  │  function_breakpoint  ├ each │
 │      parent_ │   │  elves_[2] = libmeow.so  │  │  line_breakpoint      ┘ has  │
 │      ->notify│   │  elves_[…] = libc, …     │  │      └─ on_hit_             │
 │      _hit()  │   │                          │  │      └─ breakpoint_sites_   │
 │              │   │  each elf owns a dwarf   │  │           (non-owning)      │
 │ read_memory  │   │  (line tables, DIEs, CFI)│  └─────────────────────────────┘
 │ read_memory_ │   └──────────────────────────┘                │
 │  as<T>       │                                               │ points into
 │ get_auxv     │◄──────────────────────────────────────────────┘
 │ breakpoint_  │        stoppoint_collection<breakpoint_site> (owning, on process)
 │  sites_      │
 └──────────────┘
```

### 13.2 The circular dependency, and how it is broken

```
   process ──── target_* ────► target        (process.hpp:242, set via set_target)
   target  ──── process_ ────► process       (target.hpp:120, owns)

   breakpoint_site ── parent_ ──► breakpoint  (breakpoint_site.hpp:45)
   breakpoint ── breakpoint_sites_ ──► breakpoint_site  (NON-owning:
                                       stoppoint_collection<breakpoint_site, false>)
   process ── breakpoint_sites_ ──► breakpoint_site      (OWNING)
```

Every cycle is broken by making exactly one direction a raw back-pointer, and by the
`Owning` template parameter on `stoppoint_collection`: `process` owns the sites
(`Owning = true`), while `breakpoint` merely indexes the subset it resolved
(`Owning = false`). `target` owns the `breakpoint`s. So the ownership tree is strictly:

```
   target ─owns─► process ─owns─► breakpoint_site
      └───owns──► breakpoint ─refs─► breakpoint_site
      └───owns──► elf_collection ─owns─► elf ─owns─► dwarf
```

### 13.3 Layer diagram

```
 ┌───────────────────────────────────────────────────────────────────────────┐
 │ L4  CLI              tools/gsdb.cpp                                       │
 │                      backtrace / breakpoint set / stepi / …               │
 ├───────────────────────────────────────────────────────────────────────────┤
 │ L3  SYMBOLIC         target        breakpoint (+ hit handler)             │
 │                      stack         elf_collection                         │
 │       ◄── library tracing lives entirely at this layer ──►                │
 ├───────────────────────────────────────────────────────────────────────────┤
 │ L2  DEBUG INFO       elf → dwarf → compile_unit → die / line_table / cfi  │
 │                      each elf independent, each with its own load_bias    │
 ├───────────────────────────────────────────────────────────────────────────┤
 │ L1  ADDRESSES        virt_addr ⇄ file_addr ⇄ file_offset                  │
 │                      to_file_addr(elf_collection) = the multi-ELF bridge  │
 ├───────────────────────────────────────────────────────────────────────────┤
 │ L0  PROCESS          process   breakpoint_site   registers   watchpoint   │
 │                      ptrace, process_vm_readv, /proc/<pid>/auxv           │
 └───────────────────────────────────────────────────────────────────────────┘
```

The only intrusion into L0 was the six-line `notify_hit()` block in `wait_on_signal()`.
Everything else is additive at L1–L3. That is a good outcome for a feature this invasive.

---

## 14. Verified behavior

### Build and test suite

```
$ cmake -S . -B build && cmake --build build     → BUILD_OK  (no warnings; -Werror is on)
$ cd build && ctest
100% tests passed, 0 tests failed out of 31
      Start 30: Shared library tracing works ..............   Passed    0.04 sec
```

### The `[dynlib]` test — `test/tests.cpp:885-903`

```cpp
TEST_CASE("Shared library tracing works", "[dynlib]") {
    auto dev_null = open("/dev/null", O_WRONLY);
    auto path = std::string(TARGETS_DIR) + "/marshmallow";
    auto target = target::launch(path, dev_null);
    auto& proc = target->get_process();

    target->create_function_breakpoint("libmeow_client_is_cute").enable();
    proc.resume();
    proc.wait_on_signal();

    REQUIRE(target->get_stack().frames().size() == 2);
    REQUIRE(target->get_stack().frames()[0].func_die.name().value() == "libmeow_client_is_cute");
    REQUIRE(target->get_stack().frames()[1].func_die.name().value() == "main");
    REQUIRE(target->get_pc_file_address().elf_file()->path().filename() == "libmeow.so");
}
```

Four independent things must all be right for this to pass:

| Assertion | What it actually proves |
|---|---|
| breakpoint created *before* the library is loaded, then hit | entry-point bp → rendezvous → `reload_dynamic_libraries` → `breakpoints_.for_each(resolve)` all fired in order |
| `frames().size() == 2` | `stack::unwind`'s `while (… and elf)` no longer terminates at the main-ELF boundary |
| `frames()[1].func_die.name() == "main"` | CFI unwinding produced a return address that `to_file_addr(elves_)` correctly attributed to `marshmallow`, and *its* DWARF resolved the DIE |
| `get_pc_file_address().elf_file()->path().filename() == "libmeow.so"` | `elf_collection::get_elf_containing_address` picked the right object out of ~8 |

### Build wiring — `test/targets/CMakeLists.txt`

```cmake
add_test_cpp_target(marshmallow)                 # -g -O0 -pie -gdwarf-4
add_library(meow SHARED "libmeow.cpp")           # SHARED, not static
target_compile_options(meow PRIVATE -g -O0 -fPIC -gdwarf-4)
target_link_libraries(marshmallow PRIVATE meow)
```

`SHARED` is what makes this a `.so` rather than an archive folded into the executable;
`-fPIC` is what makes the library's code position-independent so a nonzero load bias is
legal; `-gdwarf-4` matches the parser's hard version constraint. The DWARF reader throws on
anything else (`src/dwarf.cpp:444-450`: *"Only DWARF32 is supported!"*, *"Only DWARF
version 4 is supported!"*, *"Invalid address size for DWARF!"*) from `parse_compile_units`,
which the `dwarf` constructor calls eagerly, which the `elf` constructor calls at
`src/elf.cpp:60`. So **a library built with `-gdwarf-5` would make `new elf(path)` throw
out of `reload_dynamic_libraries()`** — i.e. out of a breakpoint hit handler, i.e. out of
`wait_on_signal`. On this system that does not bite, because the Nix-store system libraries
carry no `.debug_info` at all, so there are no compile units to reject.

### Manually verified beyond the test suite

| Scenario | Result |
|---|---|
| `breakpoint set libmeow_client_is_cute` before load, via `DT_NEEDED` | resolves at first `continue`; stops at `0x7ffff7fb60fd` with `libmeow.cpp:3` source shown |
| Same, via runtime `dlopen` (`_dl_debug_state` path — **untested by ctest**) | resolves during `dlopen`; stops at `0x7ffff7e870fd`; 2-frame backtrace `libmeow.so`… → `lateload`main` |
| vDSO dump | valid 18-section ELF, 7176 bytes = `e_shoff 6024 + 64×18` ✔ |
| `dlopen` that **fails** at relocation (`RTLD_NOW`, missing symbol) | library still appeared in the link map long enough to be picked up; site was created at `0x7ffff7e870fd` and never fired |
| `dlclose` | site remains listed as `enabled` at an address that is no longer mapped — no unload path (see below) |

---

## 15. Gaps, deviations, and observations

Ordered by how much they matter.

### 15.1 `r_state` is never checked — plan step 4 is half-implemented

`README.md` specifies: *"Whenever we hit the `_dl_debug_state` function breakpoint **and
the `r_state` member of the rendezvous structure is `RT_CONSISTENT`**, reread `r_map`,
adding any new shared libraries **and unloading any ones that were removed**."*

Neither clause is implemented. `reload_dynamic_libraries()` (`src/target.cpp:416-465`)
reads `r_debug` but ignores `r_state`, and has no removal logic at all.

Consequences:

- **Runs twice per `dlopen`.** glibc calls `_dl_debug_state` once with `RT_ADD` and once
  with `RT_CONSISTENT`. Both invocations run the full walk. Harmless (adds are idempotent),
  just wasted work.
- **May observe a mid-mutation link map.** During `RT_ADD`/`RT_DELETE` the list is being
  edited. Because the code only ever appends and always dedups by path first, a torn read
  can at worst miss a node — and the next `RT_CONSISTENT` call catches it. Fragile rather
  than broken.
- **Libraries are never unloaded.** This one is real and demonstrated:

  ```
  # after dlclose(handle):
  Current breakpoints:
  1: function = libmeow_client_is_cute, enabled:
      .1: address = 0x7ffff7e870fd, enabled     ◄── memory is no longer mapped
  ```

  The stale `elf` keeps a load bias pointing into a freed mapping, so
  `get_elf_containing_address` can attribute a *future* address to the wrong object if the
  kernel reuses that range for a later `dlopen`. The stale `breakpoint_site` still claims
  `is_enabled()`, so any subsequent `disable()` issues `PTRACE_PEEKDATA` on unmapped memory
  and throws. Adding an `r_state == RT_CONSISTENT` guard plus a reconcile-and-remove pass
  would close both.

### 15.2 `__cxa_demangle` was disabled in `function_name_at_address`

`src/target.cpp:333-336`:

```cpp
auto elf_name = std::string{obj->get_string(elf_func.value()->st_name)};   // now dead
func_name = obj->get_string(elf_func.value()->st_name);
// return abi::__cxa_demangle(elf_name.c_str(), nullptr, nullptr, nullptr);
```

Any function resolved through the **ELF symbol table** path (i.e. a library with no DWARF —
which is every system library) now reports its **mangled** name: `libc.so.6`_Z3fooi`
instead of `libc.so.6`foo(int)`. Functions resolved through DWARF are unaffected, which is
why no test noticed.

The commented-out version also had a latent bug worth preserving in memory: it returned
`char*` from `__cxa_demangle` directly into a `std::string` return value, **leaking** the
malloc'd buffer, and would have returned `nullptr` (UB on `std::string` construction) for
any name that failed to demangle — which is every plain C symbol. `elf::build_symbol_maps`
(`src/elf.cpp:182-188`) shows the correct pattern: check `demangle_status == 0`, then
`free()`.

`elf_name` is now a dead local. It survives `-Wall -Wextra -Werror` only because
`-Wunused-variable` does not fire for types with non-trivial destructors.

### 15.3 `bp.resolve()` is inside the link-map loop

`src/target.cpp:463` sits inside the `while (entry_ptr != nullptr)` body. With 7 libraries
and *B* breakpoints, `resolve()` runs 7·*B* times per reload instead of *B*. Every
`resolve()` override re-runs `find_functions` (which is `elves_.for_each` × DWARF index
lookup) or `get_line_entries_by_line` (which is `elves_ × compile_units × line-table
scan`). So the cost is roughly `O(N_libs² × N_breakpoints × N_CUs)`. Moving the
`for_each` one closing brace down is a one-character-position fix.

### 15.4 `wait_on_signal` recurses instead of looping

`src/process.cpp:209-210`: `resume(); return wait_on_signal();`. One stack frame per
auto-restarted internal stop. Depth is 2 per `dlopen` in practice, but a plugin host that
loads modules in a loop nests linearly. Note this predates the shared-library work —
`maybe_resume_from_syscall` (`src/process.cpp:685-686`) does the same thing for filtered
syscalls. Converting both to a `while (true)` around the body would bound the stack.

### 15.5 `read_memory(name_addr, 4096)` over-reads

`src/target.cpp:432` unconditionally requests 4096 bytes for a NUL-terminated path.
`process::read_memory` (`src/process.cpp:452-463`) splits the request at page boundaries
into multiple remote `iovec`s, and `process_vm_readv` only returns `-1` if the *first*
iovec fails — so a string near the end of a mapping yields a short read rather than an
error. That makes this safe in practice but by accident, not by construction. A path
sitting in the last few bytes of the final mapped page of a region would still be read
correctly; only a first-page failure throws.

There is also no bound on the `std::filesystem::path` construction: if the 4096 bytes
happen to contain no NUL, `path{reinterpret_cast<char*>(...)}` reads past the buffer.
In practice `l_name` always points at a NUL-terminated `DT_STRTAB` string, so this is
theoretical.

### 15.6 `dump_vdso` leaks a temp directory per session

`mkdtemp` creates a fresh `/tmp/gsdb-XXXXXX` on every debug session and nothing ever
removes it. After my testing:

```
$ ls -d /tmp/gsdb-* | wc -l
18
$ ls -l /tmp/gsdb-1M9pin/
-rw-r--r-- 1 rollschild users 7176 … linux-vdso.so.1
```

Also, `mkdtemp`'s return value is unchecked — on failure `tmp_dir` is left as the literal
`"/tmp/gsdb-XXXXXX"` and the subsequent `ofstream` silently fails, then `new elf(path)`
throws `Could NOT open ELF file!`. A `~target` cleanup (or a single per-process temp dir)
plus an `if (!mkdtemp(tmp_dir)) error::send_errno(...)` would tidy this.

### 15.7 `get_elf_containing_address` scans non-allocated sections

`elf::get_section_containing_address(virt_addr)` (`src/elf.cpp:140-150`) loops **all**
section headers with no `SHF_ALLOC` filter. Sections like `.debug_info`, `.symtab`, and
`.comment` have `sh_addr == 0`, so they claim the range
`[load_bias, load_bias + sh_size)` — which is inside that object's own first `LOAD`
segment. So the *object* returned is still correct (benign), but the *section* returned can
be a non-loaded one. Since `to_file_addr` only uses the result as a boolean validity test,
nothing breaks today. Filtering on `sh_flags & SHF_ALLOC` would make it correct rather than
lucky.

Separately, the lookup is `O(N_elves × N_sections)` on a path (`get_pc_file_address`) that
runs on **every single stop and every single instruction step**. For 8 objects averaging 30
sections that is ~240 comparisons per step. An interval tree, or just caching the last hit,
would matter for `step_over` across a long line.

### 15.8 `resolve_dynamic_linker_rendezvous` assumes `.dynamic` exists

`src/target.cpp:350-353` calls `dynamic_section.value()` on the `std::optional` without
checking. Debugging a **statically linked** executable — which has no `.dynamic` section —
throws `std::bad_optional_access` out of `target::launch`'s hit handler, i.e. out of
`wait_on_signal`. An early `if (!dynamic_section) return;` would make static binaries work
again. Worth noting the entry-point breakpoint is installed unconditionally in `launch()`,
so this fires on the first `continue` of *any* static binary.

### 15.9 The entry-point breakpoint is never removed

After the rendezvous is resolved, the `int3` at `AT_ENTRY` stays patched forever, and its
handler keeps firing (the idempotence guard at `src/target.cpp:348` makes it a no-op that
returns `true`). Costs one `int3` re-patch cycle on the vanishingly rare occasion that
`AT_ENTRY` executes twice. Harmless, but a `breakpoints_.remove_by_id()` after first
resolution would be tidier.

### 15.10 Handler lambda capture styles differ

```cpp
// src/target.cpp:77  — explicit capture of a raw pointer
entry_bp.install_hit_handler([target = tgt.get()] { … });

// src/target.cpp:382 — implicit capture of `this` by reference
debug_state_bp.install_hit_handler([&] { reload_dynamic_libraries(); return true; });
```

Both are safe here — `[&]` in a member function captures only `this`, and the `target`
outlives every breakpoint it owns. But `[&]` reads as "captures the surrounding locals by
reference", which would be a dangling-capture bug if the lambda ever touched
`debug_state_addr` or `debug_info`. Writing `[this]` would make the intent unambiguous.

---

## 16. Appendix — quick reference

### 16.1 File-by-file change map

| File | Lines | What |
|---|---|---|
| `include/libgsdb/elf.hpp` | 134-162 | **new** `elf_collection` + `for_each` templates |
| `include/libgsdb/target.hpp` | 5 | `#include <link.h>` |
| | 48-49 | `get_elf()` retargeted to `*main_elf_` |
| | 95 | `read_dynamic_linker_rendezvous()` decl |
| | 97-103 | `get_elves()`, `get_main_elf()`, `get_line_entries_by_line()` |
| | 106-109 | ctor: `main_elf_ = obj.get()` then `elves_.push(move(obj))` |
| | 117-118 | `resolve_dynamic_linker_rendezvous()`, `reload_dynamic_libraries()` |
| | 126-129 | `dynamic_linker_rendezvous_address_`, `elves_`, `main_elf_` |
| `include/libgsdb/types.hpp` | 18, 53 | fwd-declare `elf_collection`; `to_file_addr(const elf_collection&)` |
| `include/libgsdb/breakpoint.hpp` | 7, 68-81, 99 | `<functional>`, `install_hit_handler`, `notify_hit`, `on_hit_` |
| `src/target.cpp` | 46-63 | **new** `dump_vdso()` |
| | 74-83 | entry-point internal breakpoint + handler in `launch()` |
| | 98 | direct `resolve_dynamic_linker_rendezvous()` in `attach()` |
| | 102-104 | `get_pc_file_address` → `to_file_addr(elves_)` |
| | 276-294 | `find_functions` → `elves_.for_each` |
| | 316-344 | `function_name_at_address` → `elf`func` format |
| | 346-390 | **new** `resolve_dynamic_linker_rendezvous()` |
| | 395-406 | **new** `get_line_entries_by_line()` |
| | 408-414 | **new** `read_dynamic_linker_rendezvous()` |
| | 416-465 | **new** `reload_dynamic_libraries()` |
| `src/elf.cpp` | 274-301 | `elf_collection` lookups |
| `src/types.cpp` | 26-31 | `to_file_addr(const elf_collection&)` |
| `src/process.cpp` | 196-212 | `contains_address` + `parent_->notify_hit()` + auto-restart |
| `src/breakpoint.cpp` | 93-125 | `line_breakpoint::resolve` de-nested onto `get_line_entries_by_line` |
| `src/stack.cpp` | 79 | `while (… and elf)` — no longer stops at the main-ELF boundary |
| | 104 | `to_file_addr(target_->get_elves())` |
| `test/tests.cpp` | 826-861 | stepping assertions updated to `step`main` etc. |
| | 885-903 | **new** `[dynlib]` test case |
| `test/targets/CMakeLists.txt` | — | `marshmallow` target, `meow` SHARED lib, `-fPIC`, link |
| `test/targets/marshmallow.cpp` | — | **new** executable that calls into the library |
| `test/targets/libmeow.cpp` | — | **new** shared library that reads a symbol from the executable |

### 16.2 Dynamic-tag / struct-offset cheat sheet

| Tag | Value | Meaning | Read by gsdb? |
|---|---|---|---|
| `DT_NEEDED` | 1 | name of a required library | no — the linker handles it |
| `DT_DEBUG` | **21 (0x15)** | **runtime-written `&r_debug`** | **yes** — `src/target.cpp:370` |
| `DT_PLTGOT` | 3 | GOT/PLT address | no |
| `DT_FLAGS` | 30 | e.g. `BIND_NOW` | no |

| Struct | Offset | Field | Used by gsdb for |
|---|---|---|---|
| `r_debug` | +8 | `r_map` | head of the link-map walk |
| | +16 | `r_brk` | address of `_dl_debug_state` → internal breakpoint |
| | +24 | `r_state` | **not used** (gap 15.1) |
| `link_map` | +0 | `l_addr` | **the load bias** → `elf::notify_loaded` |
| | +8 | `l_name` | path → `new elf(path)` / dedup key |
| | +24 | `l_next` | iteration |

### 16.3 The three internal-breakpoint kinds now in play

| Created by | Address | `internal` | `parent_` | Handler returns | Visible? |
|---|---|---|---|---|---|
| `target::launch` | `auxv[AT_ENTRY]` | yes (`id == −1`) | set | `true` | no |
| `resolve_dynamic_linker_rendezvous` | `r_debug.r_brk` (`_dl_debug_state`) | yes (`id == −1`) | set | `true` | no |
| `target::run_until_address` | stepping target | yes | **`nullptr`** | n/a | no (removed after use) |
| any user `breakpoint set` | resolved site(s) | no | set | no handler → `false` | **yes** |

### 16.4 One-paragraph summary

`target` stopped owning one ELF and started owning an `elf_collection`. At launch it plants
an invisible breakpoint on `auxv[AT_ENTRY]` — the moment the dynamic linker has finished
its initial work — whose hit handler reads the main executable's `.dynamic` section *out of
process memory* (not out of the file, because `DT_DEBUG`'s pointer is written at runtime),
extracts `&r_debug`, walks the linker's `link_map` linked list to build one `elf` per loaded
object using `l_addr` as that object's load bias, dumps the file-less vDSO to `/tmp` so it
can be treated like any other library, and finally plants a second invisible breakpoint on
`_dl_debug_state` so every later `dlopen` repeats the walk. Each of those handlers returns
`true`, which tells `wait_on_signal` to silently resume — the user never sees the stops.
Every load also re-runs `resolve()` on all user breakpoints, so a breakpoint typed against
a library that does not exist yet materializes the instant that library appears. The one
piece of glue that makes all of it usable is
`virt_addr::to_file_addr(const elf_collection&)`: give it a raw PC and it finds the owning
object and subtracts *that* object's bias — which is why source-level stepping, line
lookup, and CFI unwinding all started working across library boundaries without a single
change to `step_in`, `step_over`, or `step_out`.
