# gsdb Architecture Walkthrough

This document is a guided map of the current `gsdb` project: a Linux debugger
written in C++23 around `ptrace`, aimed at x86-64 ELF programs.

It focuses on how the project is shaped today, what each part owns, and how the
major runtime paths fit together.

## 1. Big Picture

```mermaid
flowchart TB
    classDef user fill:#f8fafc,stroke:#334155,stroke-width:1px,color:#0f172a
    classDef build fill:#ecfeff,stroke:#0891b2,stroke-width:1px,color:#0f172a
    classDef cli fill:#eef2ff,stroke:#4f46e5,stroke-width:1px,color:#111827
    classDef lib fill:#f0fdf4,stroke:#16a34a,stroke-width:1px,color:#111827
    classDef kernel fill:#fff7ed,stroke:#ea580c,stroke-width:1px,color:#111827
    classDef target fill:#fef2f2,stroke:#dc2626,stroke-width:1px,color:#111827
    classDef test fill:#faf5ff,stroke:#9333ea,stroke-width:1px,color:#111827

    User["Developer / CLI user"]:::user
    Nix["flake.nix<br/>dev shell deps"]:::build
    CMake["CMake build<br/>lib + CLI + tests"]:::build
    Lib["gsdb::libgsdb<br/>static debugger library"]:::lib
    CLI["tools/gsdb<br/>libedit REPL"]:::cli
    Tests["test/tests<br/>Catch2 suite"]:::test
    CTest["CTest<br/>discovered test cases"]:::test
    Targets["test/targets<br/>tiny debuggee binaries"]:::test
    Process["gsdb::process<br/>inferior lifecycle"]:::lib
    Registers["gsdb::registers<br/>register cache + writes"]:::lib
    Breakpoints["breakpoint_site<br/>software int3 breakpoints"]:::lib
    Stoppoints["stoppoint_collection<br/>generic indexed storage"]:::lib
    Pipe["gsdb::pipe<br/>exec errors + test stdout"]:::lib
    Metadata["register_info<br/>X-macro register database"]:::lib
    Kernel["Linux kernel<br/>ptrace + waitpid + signals"]:::kernel
    Inferior["Inferior process<br/>debugged program"]:::target

    User --> CLI
    Nix --> CMake
    CMake --> Lib
    CMake --> CLI
    CMake --> Tests
    CMake --> CTest
    CMake --> Targets

    CLI -->|commands| Lib
    Tests -->|API checks| Lib
    CTest --> Tests
    Tests --> Targets

    Lib --> Process
    Process --> Registers
    Process --> Breakpoints
    Process --> Pipe
    Process --> Kernel
    Registers --> Metadata
    Registers --> Kernel
    Breakpoints --> Stoppoints
    Breakpoints --> Kernel
    Kernel <--> Inferior
```

The important separation is:

- `tools/gsdb.cpp` is the user-facing shell.
- `src/` and `include/libgsdb/` are the reusable debugger engine.
- `test/tests.cpp` drives the engine directly, using small binaries in
  `test/targets/` as debuggee processes.
- The kernel is part of the architecture. Most real debugger behavior happens
  through `ptrace`, `waitpid`, process signals, `/proc`, and x86-64 register
  structures.

## 2. Directory Map

```txt
gsdb/
  CMakeLists.txt                root build, CTest, compile_commands symlink
  flake.nix

  include/libgsdb/
    process.hpp                 public process API
    target.hpp                  owns a process plus its loaded elf
    stack.hpp                   stack_frame, inline height, CFI unwind driver
    registers.hpp               register cache and typed access API
    register_info.hpp           register metadata lookup helpers
    detail/registers.inc        X-macro register table
    detail/dwarf.h              raw DW_* constant enums
    breakpoint.hpp              address/function/line breakpoint objects
    breakpoint_site.hpp         one software or hardware breakpoint site
    watchpoint.hpp              hardware watchpoint
    stoppoint_collection.hpp    generic container for breakpoints/watchpoints
    disassembler.hpp            Zydis wrapper
    elf.hpp                     mmap'd ELF parser and symbol tables
    dwarf.hpp                   DWARF parser, line table, CFI
    syscalls.hpp                syscall name/id mapping
    pipe.hpp                    small RAII pipe wrapper
    parse.hpp                   CLI value parsing helpers
    bit.hpp                     byte conversion helpers
    types.hpp                   virt_addr, file_addr, file_offset, span,
                                byte64, byte128, stoppoint_mode
    error.hpp                   exception type

  src/
    process.cpp                 ptrace lifecycle, wait, resume, attach, launch
    target.cpp                  process+elf coordinator, source-level stepping
    stack.cpp                   inline stack, frame list, unwind
    registers.cpp               read/write logic for GPR/FPR/debug registers
    breakpoint.cpp              breakpoint resolution to sites
    breakpoint_site.cpp         patch/unpatch int3 breakpoint bytes
    watchpoint.cpp              hardware watchpoint slots and value tracking
    disassembler.cpp            Zydis decode from inferior memory
    elf.cpp                     ELF header/section/symbol parsing
    dwarf.cpp                   DIEs, abbrevs, line table, ranges, CFI
    types.cpp                   address-type conversions
    syscalls.cpp                syscall table lookups
    pipe.cpp                    pipe2/read/write/close wrapper
    include/syscalls.inc        private X-macro syscall table

  tools/
    gsdb.cpp                    libedit REPL and command dispatch

  test/
    CMakeLists.txt              Catch2 executable and CTest discovery
    tests.cpp                   Catch2 tests for process/register/breakpoint/
                                memory/watchpoint/syscall/elf/dwarf/target
    targets/
      CMakeLists.txt            debuggee target builders
      *.cpp, *.s                small tracee binaries
```

> **Scope note.** Sections 4–11 below describe the `process`-centred core as it
> stood before the ELF, DWARF, `target`, and `stack` layers landed. They are
> still accurate about that core, but `process` is no longer the whole spine —
> `target` now owns a `process` plus its `elf`, and drives source-level stepping.
> See `.codex/elf-support-walkthrough.md` and
> `.codex/dwarf-line-table-walkthrough.md` for those layers.

## 3. Build and Link Shape

```mermaid
flowchart LR
    classDef source fill:#f8fafc,stroke:#475569,color:#111827
    classDef target fill:#ecfdf5,stroke:#059669,color:#111827
    classDef external fill:#fff7ed,stroke:#ea580c,color:#111827

    Headers["include/libgsdb/*.hpp"]:::source
    Src["src/*.cpp"]:::source
    ToolSrc["tools/gsdb.cpp"]:::source
    TestSrc["test/tests.cpp"]:::source
    TargetSrc["test/targets/*"]:::source
    RootCMake["CMakeLists.txt<br/>include(CTest)"]:::source
    TestCMake["test/CMakeLists.txt<br/>include(Catch)"]:::source

    Lib["libgsdb.a<br/>CMake target: gsdb::libgsdb"]:::target
    CLI["build/tools/gsdb"]:::target
    TestExe["build/test/tests"]:::target
    Tracees["build/test/targets/*"]:::target
    CTest["CTest cases<br/>one per Catch2 TEST_CASE"]:::target
    CompileDb["compile_commands.json<br/>root symlink to build tree"]:::target

    Libedit["PkgConfig::libedit"]:::external
    Catch2["Catch2::Catch2WithMain"]:::external
    CatchDiscover["catch_discover_tests"]:::external

    RootCMake --> Lib
    RootCMake --> CLI
    RootCMake --> TestExe
    RootCMake --> CompileDb
    Headers --> Lib
    Src --> Lib
    ToolSrc --> CLI
    Lib --> CLI
    Libedit --> CLI

    TestSrc --> TestExe
    Lib --> TestExe
    Catch2 --> TestExe
    TargetSrc --> Tracees
    TestCMake --> TestExe
    TestCMake --> CatchDiscover
    TestExe --> CatchDiscover
    CatchDiscover --> CTest
    Tracees -. TARGETS_DIR .-> TestExe
```

The library is the center of the project. The CLI and tests both link against
`gsdb::libgsdb`, but the CLI also links `libedit`, and the tests link Catch2.
The root build includes CTest and gates the test subdirectory with
`BUILD_TESTING`; `test/CMakeLists.txt` includes Catch2's CMake helper and calls
`catch_discover_tests(tests)`, so `ctest` sees each Catch2 `TEST_CASE` as its own
test. The root CMake file also exports `compile_commands.json` and creates a
project-root symlink for clangd when one is not already present.

## 4. Runtime Object Model

```mermaid
classDiagram
    direction LR

    class process {
      +launch(path, debug, stdout_replacement)
      +attach(pid)
      +resume()
      +wait_on_signal()
      +step_instruction()
      +get_registers()
      +get_pc()
      +set_pc(address)
      +create_breakpoint_site(address)
      -pid_
      -state_
      -terminate_on_end_
      -is_attached_
      -read_all_registers()
    }

    class registers {
      +read(register_info)
      +write(register_info, value)
      +read_by_id_as_T(register_id)
      +write_by_id(register_id, value)
      -user data_
      -process* proc_
    }

    class breakpoint_site {
      +id()
      +enable()
      +disable()
      +is_enabled()
      +address()
      -process* process_
      -virt_addr address_
      -std::byte saved_data_
    }

    class stoppoint_collection~T~ {
      +push(unique_ptr_T)
      +contains_id(id)
      +contains_address(address)
      +enabled_stoppoint_at_address(address)
      +get_by_id(id)
      +get_by_address(address)
      +remove_by_id(id)
      +remove_by_address(address)
      +for_each(f)
      +size()
      +empty()
    }

    class register_info {
      +register_id id
      +string_view name
      +int32 dwarf_id
      +size_t size
      +size_t offset
      +register_type type
      +register_format format
    }

    class pipe {
      +read()
      +write(bytes)
      +close_read()
      +close_write()
      +release_read()
      +release_write()
    }

    process "1" *-- "1" registers
    process "1" *-- "1" stoppoint_collection~breakpoint_site~
    stoppoint_collection~breakpoint_site~ "1" *-- "*" breakpoint_site
    registers ..> register_info
    process ..> pipe
    breakpoint_site ..> process
```

The most important ownership rule is that `process` owns the live debugging
session. It owns the register cache and the breakpoint collection. Breakpoints
store a back pointer to the owning process so they can call `ptrace` against the
right PID when enabling or disabling themselves.

## 5. Launch Flow

```mermaid
sequenceDiagram
    participant CLI as tools/gsdb or tests
    participant Proc as gsdb::process
    participant Pipe as gsdb::pipe
    participant Child as child process
    participant Kernel as Linux kernel

    CLI->>Proc: process::launch(path, debug=true)
    Proc->>Pipe: create O_CLOEXEC pipe
    Proc->>Kernel: fork()

    alt child path
        Proc->>Child: close read end
        Child->>Kernel: personality(ADDR_NO_RANDOMIZE)
        Child->>Kernel: ptrace(PTRACE_TRACEME)
        Child->>Kernel: execlp(path)
        Kernel-->>Child: exec succeeds, pipe closes on exec
        Kernel-->>Proc: SIGTRAP stop after exec
    else parent path
        Proc->>Pipe: close write end
        Proc->>Pipe: read launch error channel
        alt child wrote error
            Pipe-->>Proc: error text
            Proc-->>CLI: throw gsdb::error
        else no error
            Proc->>Kernel: waitpid(child)
            Kernel-->>Proc: stopped at exec SIGTRAP
            Proc->>Proc: read_all_registers()
            Proc-->>CLI: process handle
        end
    end
```

The launch path uses a pipe for clean error reporting across `fork` and `exec`.
If `ptrace` or `exec` fails in the child, the child writes a text error into the
pipe before exiting. If `exec` succeeds, `O_CLOEXEC` closes the pipe
automatically and the parent sees no error data.

## 6. Attach Flow

```mermaid
sequenceDiagram
    participant CLI as tools/gsdb -p PID
    participant Proc as gsdb::process
    participant Kernel as Linux kernel
    participant Target as existing target process

    CLI->>Proc: process::attach(pid)
    Proc->>Kernel: ptrace(PTRACE_ATTACH, pid)
    Kernel-->>Target: deliver stop
    Proc->>Kernel: waitpid(pid)
    Kernel-->>Proc: stopped status
    Proc->>Kernel: PTRACE_GETREGS / GETFPREGS / PEEKUSER
    Kernel-->>Proc: register data
    Proc-->>CLI: process handle
```

Attached processes are detached when the `process` object is destroyed, while
launched debuggee processes are killed on destruction. That distinction lives in
the `terminate_on_end_` and `is_attached_` flags.

## 7. Continue and Breakpoint Flow

```mermaid
sequenceDiagram
    participant User as user
    participant CLI as REPL
    participant Proc as process
    participant BP as breakpoint_site
    participant Kernel as Linux ptrace/waitpid
    participant Inferior as inferior

    User->>CLI: continue
    CLI->>Proc: resume()

    alt PC is at an enabled breakpoint
        Proc->>BP: disable()
        BP->>Kernel: PEEKDATA original word
        BP->>Kernel: POKEDATA restore saved byte
        Proc->>Kernel: PTRACE_SINGLESTEP
        Kernel-->>Inferior: execute original instruction once
        Proc->>Kernel: waitpid()
        Kernel-->>Proc: stopped after single-step
        Proc->>BP: enable()
        BP->>Kernel: POKEDATA write 0xcc
    end

    Proc->>Kernel: PTRACE_CONT
    Kernel-->>Inferior: run
    CLI->>Proc: wait_on_signal()

    alt inferior hits int3 breakpoint
        Inferior-->>Kernel: SIGTRAP
        Kernel-->>Proc: waitpid stopped
        Proc->>Kernel: read registers
        Proc->>Proc: instr_begin = RIP - 1
        Proc->>Proc: if breakpoint at instr_begin, set RIP back
        Proc-->>CLI: stopped at breakpoint address
    else inferior exits
        Kernel-->>Proc: exited status
        Proc-->>CLI: process_state::exited
    else inferior receives signal
        Kernel-->>Proc: stopped/terminated status
        Proc-->>CLI: stop_reason
    end
```

Software breakpoints are implemented by replacing the first byte at an address
with `0xcc`, the x86-64 `int3` instruction. When the CPU executes `int3`, `rip`
has already advanced by one byte, so `wait_on_signal()` rewinds `rip` back to
the breakpoint address when it recognizes one of its own breakpoint sites.

## 8. Register Subsystem

```mermaid
flowchart TB
    classDef meta fill:#eef2ff,stroke:#4f46e5,color:#111827
    classDef cache fill:#f0fdf4,stroke:#16a34a,color:#111827
    classDef kernel fill:#fff7ed,stroke:#ea580c,color:#111827
    classDef cli fill:#f8fafc,stroke:#475569,color:#111827

    Inc["detail/registers.inc<br/>DEFINE_REGISTER rows"]:::meta
    Enum["register_id enum"]:::meta
    Infos["g_register_infos[]"]:::meta
    Lookup["register_info_by_id/name/dwarf"]:::meta

    Stop["process stopped"]:::cli
    ReadAll["process::read_all_registers()"]:::cache
    Cache["registers::data_<br/>Linux user struct cache"]:::cache
    Read["registers::read(info)"]:::cache
    Write["registers::write(info, value)"]:::cache

    GetRegs["PTRACE_GETREGS<br/>GPRs"]:::kernel
    GetFp["PTRACE_GETFPREGS<br/>x87/MMX/SSE"]:::kernel
    PeekUser["PTRACE_PEEKUSER<br/>debug registers"]:::kernel
    SetFp["PTRACE_SETFPREGS"]:::kernel
    PokeUser["PTRACE_POKEUSER<br/>aligned user-area write"]:::kernel

    Inc --> Enum
    Inc --> Infos
    Infos --> Lookup

    Stop --> ReadAll
    ReadAll --> GetRegs
    ReadAll --> GetFp
    ReadAll --> PeekUser
    GetRegs --> Cache
    GetFp --> Cache
    PeekUser --> Cache

    Lookup --> Read
    Lookup --> Write
    Cache --> Read
    Write --> Cache
    Write --> SetFp
    Write --> PokeUser
```

The register table is one of the main structural wins in the project. The
X-macro file is included once to build the `register_id` enum and again to build
the metadata array. Every register entry carries:

- debugger id
- textual name
- DWARF id where applicable
- byte size
- offset inside Linux's `user` struct
- category: GPR, sub-GPR, FPR, debug register
- display/storage format

`registers::data_` is a snapshot, refreshed when the process stops. Reads come
from that cache. Writes update the cache first and then push the relevant bytes
back through `ptrace`.

## 9. CLI Command Layer

```mermaid
flowchart LR
    classDef input fill:#f8fafc,stroke:#475569,color:#111827
    classDef cmd fill:#eef2ff,stroke:#4f46e5,color:#111827
    classDef engine fill:#f0fdf4,stroke:#16a34a,color:#111827

    Readline["readline('gsdb> ')"]:::input
    Split["split by spaces"]:::cmd
    Dispatch["handle_command"]:::cmd

    Continue["continue"]:::cmd
    Step["step"]:::cmd
    Register["register read/write"]:::cmd
    Breakpoint["breakpoint list/set/enable/disable/delete"]:::cmd
    Help["help"]:::cmd

    Process["gsdb::process"]:::engine
    Regs["gsdb::registers"]:::engine
    Bps["breakpoint collection"]:::engine

    Readline --> Split --> Dispatch
    Dispatch --> Continue --> Process
    Dispatch --> Step --> Process
    Dispatch --> Register --> Regs
    Dispatch --> Breakpoint --> Bps
    Dispatch --> Help
```

The CLI is intentionally thin. It parses simple prefix commands and delegates to
the library:

- `continue` calls `process::resume()` and then `wait_on_signal()`.
- `stepi` calls `process::step_instruction()`; `step`, `next`, and `finish` call
  the source-level `target::step_in()` / `step_over()` / `step_out()`.
- `register read` formats cached register values.
- `register write` parses text and calls `registers::write()`.
- `breakpoint set` creates and enables a `breakpoint_site`.
- breakpoint id commands operate through `stoppoint_collection`.
- `watchpoint`, `memory`, `disassemble`, and `catchpoint` were added after this
  section was written and follow the same thin-dispatch shape.

## 10. Test Architecture

```mermaid
flowchart TB
    classDef test fill:#faf5ff,stroke:#9333ea,color:#111827
    classDef target fill:#fef2f2,stroke:#dc2626,color:#111827
    classDef lib fill:#f0fdf4,stroke:#16a34a,color:#111827
    classDef os fill:#fff7ed,stroke:#ea580c,color:#111827

    CTest["ctest"]:::test
    Discover["catch_discover_tests(tests)"]:::test
    Cases["16 discovered TEST_CASE entries"]:::test
    Tests["build/test/tests<br/>Catch2 executable"]:::test
    ProcTests["process tests<br/>launch, attach, resume, exit"]:::test
    RegTests["register tests<br/>read/write GPR/FPR/vector"]:::test
    BpTests["breakpoint tests<br/>create/find/remove/hit"]:::test
    StoppointTests["collection tests<br/>size, empty, iteration, removal"]:::test

    Targets["test/targets"]:::target
    Endless["run_endlessly"]:::target
    EndNow["end_immediately"]:::target
    Hello["hello_gsdb"]:::target
    RegRead["reg_read.s"]:::target
    RegWrite["reg_write.s"]:::target

    Lib["libgsdb API"]:::lib
    Pipe["pipe stdout capture"]:::lib
    Procfs["/proc/PID/stat and maps"]:::os
    Readelf["readelf -WS"]:::os

    Discover --> Cases
    CTest --> Cases
    Cases --> Tests
    Tests --> ProcTests
    Tests --> RegTests
    Tests --> BpTests
    Tests --> StoppointTests
    ProcTests --> Lib
    RegTests --> Lib
    BpTests --> Lib
    StoppointTests --> Lib

    ProcTests --> Endless
    ProcTests --> EndNow
    RegTests --> RegRead
    RegTests --> RegWrite
    BpTests --> Hello
    StoppointTests --> Endless

    RegTests --> Pipe
    BpTests --> Procfs
    BpTests --> Readelf
    Targets -. TARGETS_DIR .-> Tests
```

The tests are integration-style tests. They launch real tracee programs and
exercise real `ptrace` behavior. That is appropriate for a debugger, but it also
means the tests need a host where `ptrace` is permitted.

The test binary can still be run directly:

```console
./build/test/tests
```

CTest discovery is now wired through Catch2:

```console
ctest --test-dir build
```

`catch_discover_tests(tests)` registers each Catch2 `TEST_CASE` as a separate
CTest test. `test/tests.cpp` now holds 29 `TEST_CASE`s: process lifecycle,
register read/write, breakpoint hit/lookup/removal and collection behavior, plus
memory, watchpoint, syscall/catchpoint, ELF, DWARF, and target cases.

## 11. Mental Model

The project can be remembered in four layers:

```mermaid
flowchart TB
    A["User surface<br/>tools/gsdb REPL"] --> B["Debugger session<br/>process"]
    B --> C["State views<br/>registers + breakpoints"]
    C --> D["Kernel machinery<br/>ptrace + waitpid + signals + /proc"]
    D --> E["Inferior program<br/>the process being debugged"]
```

The `process` class is the spine:

1. It owns the debugged PID and lifecycle policy.
2. It waits for stops and translates wait statuses into `stop_reason`.
3. It refreshes the register snapshot whenever the tracee stops.
4. It owns breakpoint sites and coordinates stepping over them.
5. It is the bridge between user-facing commands and kernel-level debugger
   operations.

The rest of the project hangs off that spine:

- `registers` gives typed access to the stopped process state.
- `breakpoint_site` knows how to patch one address.
- `stoppoint_collection` indexes breakpoint-like objects.
- `pipe` handles parent/child communication for launch errors and tests.
- `register_info` describes the architecture-specific register layout.
