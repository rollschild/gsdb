# Fix: `ld: cannot find -lZycore` during build

## Symptom

```
[ 40%] Linking CXX executable gsdb
ld: cannot find -lZycore: No such file or directory
collect2: error: ld returned 1 exit status
```

## Root cause

Nix-packaged Zydis (`zydis-4.1.1`) has a broken exported CMake config. Its
target file declares the Zycore dependency by a **bare** name:

```
# .../zydis-4.1.1/lib/cmake/zydis/zydis-targets.cmake:64
INTERFACE_LINK_LIBRARIES "Zycore"
```

but Zycore only ever creates a **namespaced** imported target:

```
# .../zycore-1.5.2/lib/cmake/zycore/zycore-targets.cmake:59
add_library(Zycore::Zycore STATIC IMPORTED)   # IMPORTED_LOCATION -> libZycore.a
```

No CMake target named plain `Zycore` exists. When we link `Zydis::Zydis`, CMake
cannot resolve `"Zycore"` to a target, so it treats it as a plain library name
and emits `-lZycore`. No `-L` is ever added for the zycore lib dir, so `ld`
fails. `libZycore.a` is present on disk
(`/nix/store/6pfqsb7sjz8nvq8z9n6gihkq5vs8abf6-zycore-1.5.2/lib/libZycore.a`);
the linker just isn't told where it is.

This is a nixpkgs packaging bug, not a change in this repo. It appeared after
the zydis derivation was rebuilt/bumped (flake update, channel move, or GC
rebuild). A clean reconfigure does NOT fix it — the mismatch is deterministic.

## Recommended fix (project CMakeLists.txt) — applied at `CMakeLists.txt:88-93`

`find_package(zydis)` already runs `find_dependency(Zycore)`, so the real
`Zycore::Zycore` imported target exists after line 87. Alias it under the bare
name Zydis asks for. Add immediately after
`find_package(zydis CONFIG REQUIRED)` (CMakeLists.txt:87):

```cmake
find_package(zydis CONFIG REQUIRED)
# Work around nixpkgs zydis-4.1.1: its exported target links the bare name
# "Zycore" instead of the imported target "Zycore::Zycore", so the linker gets
# a dangling -lZycore. Alias the real imported target under the bare name.
if(TARGET Zycore::Zycore AND NOT TARGET Zycore)
  add_library(Zycore ALIAS Zycore::Zycore)
endif()
```

Then reconfigure + rebuild:

```bash
cmake -S . -B build && cmake --build build
```

## Alternative fixes

- **Nix override**: patch the zydis derivation so `zydis-targets.cmake` uses
  `Zycore::Zycore`, or `sed` the installed config in a `postInstall`. Fixes it
  for every consumer but touches the flake.
- **Pin nixpkgs back** to the revision whose zydis linked correctly (revert the
  flake.lock bump).

The CMake alias is preferred: minimal, self-contained, and harmless once
upstream/nixpkgs fixes the exported config.
