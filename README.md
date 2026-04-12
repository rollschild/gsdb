# gsdb

A debugger for Linux, written from scratch in C++.

## Run

To test registers:

```console
./build/tools/gsdb ./build/test/targets/reg_read
```

## Project Structure

### The linking flow in summary

```txt
include/libgsdb/libgsdb.hpp  (public header)
      ↓ PUBLIC include path
 src/libgsdb.cpp → [libgsdb.a]
      ↓                    ↓
 gsdb::libgsdb        gsdb::libgsdb
 + PkgConfig::libedit + Catch2::Catch2WithMain
      ↓                    ↓
 tools/gsdb            test/tests
 (CLI binary)          (test binary)
```

## Notes

On Linux systems, executable programs are encoded as **Executable and Linkable Format (ELF)** files.

The debug information format used on Linux systems is **DWARF**.

We usually don’t want to allow the execution of memory that is on the stack, as this can lead to security vulnerabilities.

The main debug syscall provided by Linux and macOS to carry out these low-level jobs is `ptrace`.

**Interrupt handlers** are kind of like signal handlers, but they’re set up in special regions of memory and interact directly with the hardware.

The register used to keep track of the current instruction is called the **program counter** or **instruction pointer**.

C++ exceptions don’t flow between processes.

Pipes are a form of buffered communication, able to retain up to 64KiB of data by default before being read from.

### Linux **process filesystem (procfs)**

Enables us to examine the processes running on a system through files.

A virtual filesystem located at `/proc`.

- `/proc/<pid>/stat` - gives high-level information about the state of a given process, such as:
  - the name of the executable the process is running
  - the PID of its parent
  - the amount of time for which it has been running
  - the current process execution state.

### Registers

#### Sets of Registers

- general purpose
  - 16 64-bit
- x87 - `long double` support
  - x87 instructions operate on a stack of values within `st0` - `st7` registers
  - **FPU (floating-point unit)** stack
  - `fld` & `fstp` for pushing/popping values from top
  - `faddp` - arithmetic operations
    - pops top two values from FPU stack,
    - adds them,
    - pushes the result
- MMX
  - SIMD (single instruction multiple data)
- SSE
  - **Streaming SIMD Extensions**
- SSE2
- AVX (Advanced Vector Extensions)
- AVX-512
- debug

Linux uses **System V ABI (SYSV ABI)**.

- `rax` Caller-saved general register; used for return values
- `rbx` Callee-saved general register
- `rcx` Used to pass the fourth argument to functions
- `rdx` Used to pass the third argument to functions
- `rsp` Stack pointer
- `rbp` Callee-saved general register; optionally used pointer to the top of the stack frame
- `rsi` Used to pass the second argument to functions
- `rdi` Used to pass the first argument to functions
- `r8` Used to pass the fifth argument to functions
- `r9` Used to pass the sixth argument to functions
- `r10`–`r11` Caller-saved general registers
- `r12`–`r15` Callee-saved general registers

Caller-saved registers can be safely written over inside a function without causing chaos. Callee-saved registers must be saved at the start of the function and restored at the end of the function if they’re to be modified. (How do I remember this though)

x64 has segment registers named `es`, `cs`, `ss`, `ds`, `fs`, and `gs`.
Compilers still use the fs and gs registers to support thread-local variables, variables that each thread of a process has its own copy of.

Two other 64-bit registers to be aware of are:

- `rip`, which stores the instruction pointer (or program counter), and
- `rflags`, which tracks various pieces of information about the state of the processor.

Today, compilers use **Streaming SIMD Extensions (SSE)** instructions instead of the **x87** instructions and registers for general floating-point operations, but x87 registers are still used for long double support because they enable higher precision than SSE.

SYSV ABI says that `long double` arguments must be passed on the function’s stack frame rather than using registers, and you can’t just use `mov` to transfer a value out of an `st` register.

#### How to Interact with Registers with `ptrace`

- `PTRACE_GETREGS` & `PTRACE_SETREGS`
  - read/write _all_ general-purpose registers at once
- `PTRACE_GETFPREGS` & `PTRACE_SETFPREGS`
  - read/write x87, MMX, and SSE registers
- `PTRACE_PEEKUSER` - read debug regisers
- `PTRACE_POKEUSER`
  - write debug registers, or
  - write single general-purpose register

#### DWARF

Each CPU architecture numbers its registers differently.

DWARF defines its own standardized numbering scheme for registers, independent of the
hardware's native encoding.

For example, on x86-64, DWARF register `0` is `rax`, register `7` is `rsp`, register `16` is `rip`, etc.

The debugger needs a mapping from DWARF register numbers to the actual register values it reads via `ptrace`.

### X-Macros

**X-Macros** allow us to maintain independent data structures whose members or operations rely on the same underlying data and must be kept in sync.

a.k.a. **include-file macro**

### Assembly

GCC defaults to AT&T syntax.

```assembly
# move 64 bits of data from rsp to rbp
movq %rsp, %rbp
```

#### How to issue syscalls in assembly

- System V ABI:
  - the syscall ID goes in `rax`;
  - subsequent arguments go in `rdi`, `rsi`, `rdx`, `r10`, `r8`, and `r9`;
  - and the return value of the syscall is stored in `rax`

### Breakpoints

#### Hardware vs. Software Breakpoints

Hardware breakpoints involve setting architecture-specific registers to produce breaks for you.

Whereas software breakpoints involve modifying the machine instructions in the process’s memory.

The number of hardware breakpoints is limited by the number of debug registers in the system. On x64, there are only four hardware breakpoint registers.

Hardware breakpoints have the powerful ability to trigger breaks if a given address is executed, written to, or read from. Software breakpoints can trigger breaks on execution only.

Hardware breakpoints also enable the debugging of program exploits that involve overwriting memory with executable code, so they can be useful in security and reverse engineering situations.

We set software breakpoints by modifying the executing code on the fly.

The x64 architecture has an **interrupt vector table** that the operating system can use to register handlers for various events, such as dividing by zero, accessing protected memory, or executing invalid opcodes.

When the processor executes the int3 instruction, it passes control to the breakpoint interrupt handler, which—in the case of Linux—signals the process with a SIGTRAP.

Interrupt vector table — The OS registers handler functions for CPU exceptions
(divide-by-zero, page faults, etc.). Each exception type has a numbered slot in this table.

In short: `int3` → CPU exception → kernel handler → SIGTRAP → ptrace notifies debugger.

To enable a breakpoint site, we need to replace the instruction at the given address with an `int3` instruction, which we encode as `0xcc`.

#### Determine where to set breakpoints

Today, many compilers produce **position-independent executables (PIEs)** _by default_. These executables don’t expect to be loaded at a specific memory address; they can be loaded anywhere and still work.

As such, memory addresses within PIEs aren’t absolute virtual addresses, they’re _offsets_ from the start of the final load address of the binary.

PIEs exist to support **address space layout randomization (ASLR)**, a protection against malicious code that takes advantage of known virtual addresses to attack programs.

To test whether a given executable is PIE: `file <executable>`. Something like `ELF 64-bit LSB pid executable`. or `ELF 64-bit LSB shared object`.

To _globally_ disable ASLR, `echo 0 > /proc/sys/kernel/randomize_va_space`.

The `personality` syscall allows you to change some execution properties for processes, like whether they’re limited to 32-bit addresses or whether ASLR is enabled.

We can find information about where the system loaded programs in memory at `/proc/<pid>/maps`.

```txt
load address of the instruction = load address of the segment + instruction's file address - segment's start file address
```

`readelf -S ./build/test/targets/hello_gsdb`

After setting the breakpoint, if you want to continue - the solution is to set the program counter back by 1 byte if we stop at a breakpoint. Then, to resume the process, we can disable the breakpoint, step over a single instruction, re-enable the breakpoint, and resume.
