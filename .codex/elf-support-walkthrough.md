# ELF Support Walkthrough

This document explains the ELF support in this codebase: what ELF is, what this
debugger uses it for, how the parser works, and how the ELF-related classes
connect to the rest of the debugger.

## What Is An ELF?

ELF means **Executable and Linkable Format**. On Linux, it is the standard file
format for:

- executable programs
- shared libraries
- relocatable object files
- core dumps

At a high level, an ELF file is a structured binary container. It contains
machine code, data, metadata, names, symbols, relocation information, dynamic
linking information, and optional debug information.

The debugger cares about ELF because the running process only gives us raw
runtime addresses, such as the instruction pointer. ELF lets the debugger answer
questions like:

- What executable file does this process come from?
- Where is the entry point?
- What function contains this address?
- What is the name of the symbol at this address?
- Where does a named section, such as `.text`, begin?

## ELF Mental Model

ELF has two major views of the same file:

```text
ELF file
|
+-- ELF header
|   global metadata:
|   entry point, architecture, offsets to tables, table sizes
|
+-- Program headers / segments
|   runtime loader view:
|   what ranges should be mapped into memory, with what permissions
|
+-- Section headers / sections
|   linker/debugger view:
|   named regions like .text, .data, .symtab, .strtab
|
+-- String tables
|   packed NUL-terminated strings used by section and symbol tables
|
+-- Symbol tables
    records for functions, variables, sections, files, etc.
```

Segments are the **execution-time** view. They tell the kernel/dynamic loader
what memory mappings to create.

Sections are the **link-time/debugger** view. They give meaningful names and
metadata to regions of the file.

This debugger mostly uses the section/symbol side. It is not trying to be an ELF
loader.

## Repository Map

The ELF implementation is concentrated in a few files:

```text
include/libgsdb/elf.hpp
    public gsdb::elf API and internal ELF indexes

src/elf.cpp
    opens, mmaps, parses, and indexes the ELF file

include/libgsdb/types.hpp
    strong address types:
    virt_addr, file_addr, file_offset

src/types.cpp
    converts between ELF file addresses and runtime virtual addresses

include/libgsdb/target.hpp
src/target.cpp
    owns both process and ELF object, computes load bias

src/process.cpp
    reads /proc/<pid>/auxv for AT_ENTRY

tools/gsdb.cpp
    uses ELF symbol lookup to print function names in stop messages

test/tests.cpp
    has ELF parser coverage around _start symbol lookup
```

## What ELF Support Does In This Debugger

ELF support is the debugger's symbolic metadata layer.

It does **not** control process execution. Process control is handled by
`gsdb::process` through `ptrace`.

ELF support does:

- map the executable file into memory
- read the ELF header
- read section headers
- build a map from section name to section header
- find string tables
- read the symbol table
- build symbol lookup indexes
- translate runtime addresses back to ELF file addresses using load bias
- find which function/symbol contains the current program counter

The main user-visible result today is that stop messages can include function
names:

```text
stopped with signal TRAP at 0x555555555180 (main)
```

That behavior is wired through `tools/gsdb.cpp`, where the current PC is passed
to `target.get_elf().get_symbol_containing_address(...)`.

## Main Architecture

```text
                 launch(path) / attach(pid)
                           |
                           v
                    gsdb::target
                    /          \
                   /            \
                  v              v
          gsdb::process       gsdb::elf
          runtime state       file metadata
          ptrace              mmap'd executable
          registers           sections
          memory              symbols
          auxv                load bias
                  \              /
                   \            /
                    v          v
                 address translation
                 virt_addr <-> file_addr
```

`target` is the coordinator. It owns:

- a `process`, which knows about the running inferior
- an `elf`, which knows about the executable file

The ELF object alone can parse static file metadata, but it needs runtime
information from `process` to compute the load bias for PIE/ASLR-aware address
translation.

## Important Classes

### `gsdb::elf`

Declared in `include/libgsdb/elf.hpp` and implemented in `src/elf.cpp`.

Responsibilities:

- owns the file descriptor
- owns the `mmap` mapping
- stores the copied ELF header
- stores copied section headers
- indexes sections by name
- stores copied symbol table entries
- indexes symbols by name
- indexes symbols by address range
- stores load bias once the executable is loaded

Key fields:

```text
fd_
    open file descriptor for the ELF file

path_
    filesystem path

file_size_
    size from fstat()

data_
    std::byte* pointing at the mmap'd file

header_
    copied Elf64_Ehdr

section_headers_
    copied vector<Elf64_Shdr>

section_map_
    unordered_map<string_view, Elf64_Shdr*>

load_bias_
    runtime relocation offset

symbol_table_
    copied vector<Elf64_Sym>

symbol_name_map_
    unordered_multimap<string_view, Elf64_Sym*>

symbol_addr_map_
    map<[file_addr start, file_addr end), Elf64_Sym*>
```

### `gsdb::target`

Declared in `include/libgsdb/target.hpp`, implemented in `src/target.cpp`.

Responsibilities:

- launch or attach to a process
- create the matching ELF object
- compute and notify the ELF object of the runtime load bias
- expose both `get_process()` and `get_elf()`

Conceptually:

```text
target::launch(path)
    process::launch(path)
    elf(path)
    auxv = process.get_auxv()
    load_bias = auxv[AT_ENTRY] - elf.header.e_entry
    elf.notify_loaded(load_bias)
```

For `attach(pid)`, the path comes from:

```text
/proc/<pid>/exe
```

### Address Types

Declared in `include/libgsdb/types.hpp`, implemented partly in `src/types.cpp`.

There are three distinct address concepts:

```text
file_offset
    raw byte offset from the start of the ELF file on disk

file_addr
    virtual address as written in the ELF file
    examples: Elf64_Shdr::sh_addr, Elf64_Sym::st_value

virt_addr
    real virtual address in the running debugged process
    example: RIP read from registers
```

The distinction matters because ASLR and PIE mean that the executable may be
loaded at a different runtime base each time.

Address conversion:

```text
virt_addr = file_addr + load_bias
file_addr = virt_addr - load_bias
```

`file_addr` also carries a pointer to the owning `elf` object. Comparisons
assert that both addresses belong to the same ELF file.

## ELF Parser Flow

The constructor `gsdb::elf::elf(path)` performs the whole parse/index setup:

```text
gsdb::elf(path)
    |
    v
open(path, O_RDONLY)
    |
    v
fstat(fd)
    |
    v
mmap(file, PROT_READ, MAP_SHARED)
    |
    v
copy ELF header from data_[0]
    |
    v
parse_section_headers()
    |
    v
build_section_map()
    |
    v
parse_symbol_table()
    |
    v
build_symbol_maps()
```

The object is RAII-style:

```text
constructor:
    open + fstat + mmap + parse

destructor:
    munmap + close
```

This avoids reading the full file into a manually allocated buffer. Instead, the
kernel maps the file into the debugger's address space, and the parser can treat
file bytes as memory.

## Header Parsing

The ELF header is always at the start of the file.

This code copies the first `sizeof(Elf64_Ehdr)` bytes into `header_`:

```text
data_ ----------------------+
                            |
                            v
+---------------------------+--------------------+
| Elf64_Ehdr                | rest of ELF file   |
+---------------------------+--------------------+
                            |
                            v
                         header_
```

The parser relies on Linux's `<elf.h>` types:

- `Elf64_Ehdr`
- `Elf64_Shdr`
- `Elf64_Sym`

That means this parser is currently written for 64-bit ELF files.

## Section Parsing

ELF section headers live in a table. The ELF header tells us where that table
starts and how many entries it has.

Important header fields:

```text
e_shoff
    file offset to section header table

e_shnum
    number of section headers

e_shentsize
    size of each section header entry

e_shstrndx
    index of the section that contains section names
```

Flow:

```text
ELF header
    |
    | e_shoff
    v
section header table in mmap'd file
    |
    v
copy entries into vector<Elf64_Shdr> section_headers_
```

The implementation handles the ELF extended-section-count case:

```text
if e_shnum == 0 and e_shentsize != 0:
    real count is section_headers[0].sh_size
```

After copying section headers, the parser builds `section_map_`.

## Section Name Lookup

Section names are not stored inline in each section header. Each section header
stores an offset into the section-name string table.

```text
section header
    sh_name = 42
        |
        v
.shstrtab + 42
        |
        v
".text\0"
```

The section-name string table is selected by `header_.e_shstrndx`.

Then:

```text
build_section_map()
    for each section header:
        name = get_section_name(section.sh_name)
        section_map_[name] = &section
```

This enables:

```cpp
get_section(".text")
get_section(".symtab")
get_section(".strtab")
```

## Section Contents

`get_section_contents(name)` returns a `span<const std::byte>` pointing directly
into the mmap'd file:

```text
section header
    sh_offset
    sh_size
        |
        v
data_ + sh_offset, size sh_size
```

No copy is made for section contents. This is efficient, but it means returned
spans are only valid while the `elf` object and its mmap are alive.

## String Tables

A string table is a byte array of NUL-terminated strings.

Example:

```text
offset 0:  "\0"
offset 1:  "_start\0"
offset 8:  "main\0"
offset 13: "printf\0"
```

ELF tables refer to strings by integer offset.

For symbols:

```text
Elf64_Sym::st_name = 8
    |
    v
.strtab + 8
    |
    v
"main"
```

This parser's `get_string(index)` uses `.strtab` if present, otherwise `.dynstr`.

More robust ELF parsers usually follow the symbol table section's `sh_link`,
because each symbol table section says which string table belongs to it. The
README also notes this.

## Symbol Table Parsing

The parser looks for:

1. `.symtab`
2. `.dynsym` if `.symtab` is absent

`.symtab` is the full static symbol table, often present in non-stripped
binaries.

`.dynsym` is the smaller dynamic symbol table needed for dynamic linking.

Flow:

```text
get_section(".symtab")
    |
    +-- found: use it
    |
    +-- missing:
          get_section(".dynsym")
              |
              +-- found: use it
              +-- missing: no symbols
```

Once a symbol table section is found:

```text
symbol count = sh_size / sh_entsize
copy bytes from data_ + sh_offset into vector<Elf64_Sym>
```

Each `Elf64_Sym` can describe a function, object, section, file, TLS object, and
other symbol types.

Important symbol fields used here:

```text
st_name
    offset into string table

st_value
    symbol address as an ELF file address

st_size
    size in bytes

st_info
    packed binding/type metadata
```

## Symbol Indexing

After reading the symbol table, `build_symbol_maps()` creates two lookup
structures.

### Name Index

```text
symbol_name_map_
    name -> Elf64_Sym*
```

It is an `unordered_multimap` because duplicate names are legitimate in ELF:

- local static function and global function can share a name
- weak and strong symbols can share a name
- same name can occur in different sections

Lookup:

```text
get_symbols_by_name(name)
    equal_range(name)
    collect all matching Elf64_Sym*
```

The code also attempts C++ demangling through `abi::__cxa_demangle`.

Important caveat: the demangled name is inserted as a `std::string_view`, then
the allocated demangled string is immediately freed. That makes demangled-name
entries dangling. Lookups by names backed by the ELF string table are safe while
the `elf` object is alive; demangled lookup is unsafe as written.

### Address Index

```text
symbol_addr_map_
    [file_addr start, file_addr end) -> Elf64_Sym*
```

The start/end range comes from:

```text
start = symbol.st_value
end   = symbol.st_value + symbol.st_size
```

Symbols are skipped for address indexing if:

- `st_value == 0`
- `st_name == 0`
- symbol type is `STT_TLS`

The map comparator sorts only by range start address.

This supports two styles of lookup:

```text
get_symbol_at_address(address)
    exact start-address match

get_symbol_containing_address(address)
    range containment match
```

## Symbol Lookup By Runtime PC

This is the most important debugger use case.

```text
process stops
    |
    v
process.get_pc()
    |
    v
virt_addr runtime_pc
    |
    v
elf.get_symbol_containing_address(runtime_pc)
    |
    v
runtime_pc.to_file_addr(elf)
    |
    v
file_addr = runtime_pc - load_bias
    |
    v
symbol_addr_map_.lower_bound(...)
    |
    v
find symbol range containing file_addr
    |
    v
print symbol name
```

This check has since moved out of the CLI into
`target::function_name_at_address()` (`src/target.cpp:272`), which first asks
DWARF for a function DIE and only falls back to the ELF symbol table:

```cpp
} else if (auto elf_func = obj->get_symbol_containing_address(file_address);
           elf_func and
           ELF64_ST_TYPE(elf_func.value()->st_info) == STT_FUNC) {
```

If the symbol is a function, its name is demangled and returned, and the CLI
appends it to the stop message.

## Load Bias

The load bias is the difference between:

- the address encoded in the ELF file
- the address where the kernel actually loaded it

For non-PIE executables this may be zero or fixed. For PIE executables and ASLR,
it changes between runs.

The code computes it using the auxiliary vector:

```text
auxv[AT_ENTRY]
    actual runtime entry point

elf.header.e_entry
    entry point address encoded in the ELF file

load_bias = auxv[AT_ENTRY] - elf.header.e_entry
```

Diagram:

```text
ELF file view:

    e_entry = 0x1180
    main    = 0x1230

Runtime process view:

    AT_ENTRY = 0x555555555180
    main     = 0x555555555230

load_bias:

    0x555555555180 - 0x1180 = 0x555555554000

conversion:

    runtime main = 0x1230 + 0x555555554000
                 = 0x555555555230
```

`target` calls:

```cpp
obj->notify_loaded(virt_addr(auxv[AT_ENTRY] - obj->get_header().e_entry));
```

After this, `elf` can convert between runtime and file addresses.

## Address Translation Architecture

```text
             gsdb::elf
       stores load_bias for object
                  |
                  v
+-----------------+-----------------+
|                                   |
v                                   v
file_addr                       virt_addr
ELF metadata address            runtime process address
symbol.st_value                 RIP / PC
section.sh_addr                 ptrace memory address
|                                   |
| to_virt_addr()                    | to_file_addr(elf)
v                                   v
addr + load_bias                addr - load_bias
```

Before converting, the code verifies that the address belongs to a known
section. If not, conversion returns a default zero address.

## How Modules Work Together

### Launch Flow

```text
user runs debugger with program path
    |
    v
target::launch(path)
    |
    v
process::launch(path)
    |
    v
create_loaded_elf(process, path)
    |
    +--> process.get_auxv()
    |
    +--> elf(path)
    |       open/fstat/mmap/parse/index
    |
    +--> compute load_bias
    |
    +--> elf.notify_loaded(load_bias)
    |
    v
target(process, elf)
```

### Attach Flow

```text
user attaches to pid
    |
    v
target::attach(pid)
    |
    v
elf path = /proc/<pid>/exe
    |
    v
process::attach(pid)
    |
    v
create_loaded_elf(process, elf path)
    |
    v
target(process, elf)
```

### Stop Message Flow

```text
inferior stops
    |
    v
process wait status becomes stop_reason
    |
    v
tools/gsdb.cpp formats stop message
    |
    v
process.get_pc()
    |
    v
target.get_elf().get_symbol_containing_address(pc)
    |
    v
symbol name appended if STT_FUNC
```

## Object Lifetime

The `elf` object owns the mmap.

```text
elf object alive
    |
    +-- data_ valid
    +-- string_view into section/string tables valid
    +-- span from get_section_contents() valid
    +-- section_map_ keys valid if backed by mmap

elf object destroyed
    |
    +-- munmap(data_)
    +-- close(fd_)
    +-- views into mapped file are invalid
```

The copied vectors remain ordinary owned memory:

- `section_headers_`
- `symbol_table_`

But many names are `string_view`s pointing into the mapped file.

## Test Coverage

The ELF parser test does three things:

```text
1. parse hello_gsdb
2. look up _start at header.e_entry as a file_addr
3. look up _start by name
4. fake a load bias and look up _start by runtime virt_addr
```

This verifies:

- section/symbol parsing works for the test binary
- name lookup works for `_start`
- exact address lookup works
- load-bias-based virtual address lookup works

## Current Limitations And Risks

This is a useful debugger-oriented parser, but not a full robust ELF parser.

Known limitations:

- It assumes 64-bit ELF structures.
- It does not validate the ELF magic bytes.
- It does not validate ELF class, endianness, ABI, or machine architecture.
- It does not bounds-check offsets and sizes before copying.
- It does not parse program headers/segments.
- It does not parse relocations.
- It does not parse DWARF debug info.
- It picks `.strtab` or `.dynstr` globally instead of following `sh_link`.
- Returned spans and string views can dangle after `elf` destruction.
- Demangled names are currently stored as dangling `string_view`s.
- Section containment uses strict lower bound for `file_addr`, so an address
  exactly equal to `section.sh_addr` is not considered contained.

The parser is therefore fine as a learning/debugger feature for known-good
Linux x86-64 test binaries, but it should be hardened before being exposed to
arbitrary untrusted ELF files.

## Summary

The ELF support gives the debugger a bridge from raw runtime addresses to human
meaning.

```text
runtime PC
    |
    v
subtract load bias
    |
    v
ELF file address
    |
    v
symbol address map
    |
    v
function name
```

The core design is simple:

- `process` knows runtime facts.
- `elf` knows static file facts.
- `target` owns both and computes the load bias.
- `types` prevents mixing runtime addresses, ELF addresses, and file offsets.
- `tools/gsdb.cpp` uses the result to display symbolic stop messages.

