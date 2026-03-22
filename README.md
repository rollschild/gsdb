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
