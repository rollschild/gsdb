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
