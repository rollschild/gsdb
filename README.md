# gsdb

A debugger for Linux, written from scratch in C++.

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
- x87
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
