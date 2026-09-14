# C++ + Nix project structure: best practices, gsdb assessment, and the template

Date: 2026-09-12. Research report only; no gsdb source files were changed.
The resulting template lives at `~/projects/cpp-nix-template` (git repo,
builds and passes `nix flake check`).

## 1. TL;DR

- There is **no official Nix C++/CMake template**. `NixOS/templates` has only
  an autotools `c-hello`; nix.dev has no C++ tutorial. Best practice is
  assembled from the nixpkgs manual (cmake hook, `mkShell`, hardening,
  `lib.fileset`), the Nix manual's flake output schema, and what the strongest
  real-world C++ flakes do (Hyprland, nixd, tenzir, NixOS/nix).
- **Consensus shape:** package derivation in its own `nix/package.nix` (nixpkgs
  style, `callPackage`d, `finalAttrs`), dev shell = `mkShell { inputsFrom = [ pkg ]; packages = [ clang-tools ... ]; hardeningDisable = [ "fortify" ]; }`,
  `packages`/`devShells`/`checks`/`formatter` outputs, nixpkgs pinned to
  `nixos-unstable` or a release channel, multi-system via plain
  `lib.genAttrs` (or flake-parts for big projects), `lib.fileset` source
  filtering, `treefmt-nix` for `nix fmt`, direnv + nix-direnv for shell entry
  and GC roots.
- **CMake side:** target-based everything, namespaced ALIAS targets,
  `FILE_SET HEADERS`, `include(CTest)` gated on `PROJECT_IS_TOP_LEVEL`,
  `catch_discover_tests`, install + export + package config, and
  **`CMakePresets.json`** (workflow presets do configure+build+test in one
  command) with a `.clangd` file pointing at `build/dev` instead of a
  `compile_commands.json` symlink.
- **gsdb** gets the CMake target hygiene mostly right (ALIAS, interface
  generator expressions, install/export, Catch2 discovery) but its Nix side and
  top-level CMake carry several anti-patterns. Details and one-line fixes in §4.

## 2. Sources

Official / primary:

- nixpkgs manual, cmake hook and variables: <https://nixos.org/manual/nixpkgs/unstable/#cmake>
- nixpkgs cmake `setup-hook.sh` (confirmed locally in `/nix/store/*-cmake-4.1.2/nix-support/setup-hook`): `-DCMAKE_INSTALL_PREFIX`, `-DCMAKE_BUILD_TYPE=${cmakeBuildType:-Release}`, `-DBUILD_TESTING=OFF` when `doCheck` unset, `-GNinja` when ninja drives the build: <https://github.com/NixOS/nixpkgs/blob/master/pkgs/by-name/cm/cmake/setup-hook.sh>
- nixpkgs manual, `mkShell` (`inputsFrom`, `packages`): <https://nixos.org/manual/nixpkgs/unstable/#sec-pkgs-mkShell>
- nixpkgs manual, hardening flags (`fortify` adds `-O2 -D_FORTIFY_SOURCE=2`): <https://nixos.org/manual/nixpkgs/unstable/#sec-hardening-in-nixpkgs>; the `-O0` interaction: <https://github.com/NixOS/nixpkgs/issues/60919>
- nix.dev, `lib.fileset`: <https://nix.dev/tutorials/working-with-local-files>; pinning and which branches are cached: <https://nix.dev/concepts/faq>, <https://nix.dev/reference/pinning-nixpkgs>
- Nix manual, flake output schema and deprecated singular names (`devShell` → `devShells.<system>.default`, `overlay` → `overlays.default`): <https://nixos.org/manual/nix/stable/command-ref/new-cli/nix3-flake-check>, <https://github.com/NixOS/nix/blob/master/src/nix/flake-check.md>
- NixOS/templates (`c-hello` uses plain `lib.genAttrs`, an overlay, `packages` + `checks`): <https://github.com/NixOS/templates>
- nix-direnv (`use flake`, GC roots under `.direnv/`, NixOS options): <https://github.com/nix-community/nix-direnv>
- treefmt-nix (`programs.clang-format`, `programs.cmake-format`, `programs.nixfmt`): <https://github.com/numtide/treefmt-nix>
- CMake presets (workflow presets need schema v6 / CMake 3.25): <https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html>
- `target_sources(FILE_SET HEADERS)` (3.23): <https://cmake.org/cmake/help/latest/command/target_sources.html>
- `PROJECT_IS_TOP_LEVEL` (3.21): <https://cmake.org/cmake/help/latest/variable/PROJECT_IS_TOP_LEVEL.html>
- CTest module / `BUILD_TESTING`: <https://cmake.org/cmake/help/latest/module/CTest.html>
- `CMAKE_<LANG>_COMPILER` is fixed at first configure: <https://cmake.org/cmake/help/latest/variable/CMAKE_LANG_COMPILER.html>
- Catch2 v3 CMake integration: <https://github.com/catchorg/Catch2/blob/devel/docs/cmake-integration.md>
- clangd config, `CompileFlags.CompilationDatabase` relative to `.clangd`: <https://clangd.llvm.org/config>
- Pitchfork Layout spec: <https://github.com/vector-of-bool/pitchfork>
- Modern CMake do's and don'ts: <https://cliutils.gitlab.io/modern-cmake/chapters/intro/dodonot.html>

Templates and real projects surveyed:

- friendlyanon/cmake-init (presets, `build/dev`, `.clangd`, `cmake/` modules): <https://github.com/friendlyanon/cmake-init>
- cpp-best-practices/cmake_template (warning/option interface targets, sanitizers, Catch2): <https://github.com/cpp-best-practices/cmake_template>
- hyprwm/Hyprland (`nix/default.nix`, `finalAttrs`, `lib.fileset`, `inputsFrom`, `hardeningDisable = ["fortify"]`, `strictDeps`): <https://github.com/hyprwm/Hyprland/blob/main/nix/default.nix>
- hyprwm/hyprutils (lib + include/src/tests, debug/with-tests variants): <https://github.com/hyprwm/hyprutils>
- nix-community/nixd (flake-parts + treefmt-nix, shell via `overrideAttrs`): <https://github.com/nix-community/nixd>
- tenzir/tenzir (`CMakePresets.json` with Ninja, `.clangd`, `nix/package.nix`, `cmake-format`): <https://github.com/tenzir/tenzir>
- NixOS/nix (per-component `package.nix` with `fileset`, `lib.genAttrs`): <https://github.com/NixOS/nix/blob/master/flake.nix>
- Smaller templates, useful mostly as anti-pattern catalogues: ALT-F4-LLC/kickstart.nix `cpp-cmake`, nixvital/nix-based-cpp-starterkit, skynet-core/flakes-cmake-project-template, nkoturovic/cpp-nix-project-template.

## 3. Best-practice checklist

Nix:

1. Package in `nix/package.nix` as a nixpkgs-style function; flake does `pkgs.callPackage ./nix/package.nix { }`. Use `stdenv.mkDerivation (finalAttrs: { ... })` and `strictDeps = true`.
2. `src = lib.fileset.toSource { root = ../.; fileset = lib.fileset.unions [ ... ]; }` so docs/flake edits do not rebuild.
3. `cmake` and `ninja` in `nativeBuildInputs`; libraries in `buildInputs`; test-only libs in `checkInputs`; `doCheck = true`. Never hand-write configure/build/install phases.
4. Dev shell: `mkShell { inputsFrom = [ pkg ]; packages = [ clang-tools ... ]; hardeningDisable = [ "fortify" ]; }`. No `exec <shell>`, no `PS1` hacks, no hidden build steps in `shellHook`.
5. Outputs: `packages.<system>.default`, `devShells.<system>.default`, `checks`, `formatter`, `overlays.default`, optionally `templates`. Never the deprecated singular names.
6. Pin `nixos-unstable` (or `nixos-YY.MM`), never `master`. Only list systems you can build.
7. Multi-system with `lib.genAttrs`; flake-parts if the flake grows modules. Skip flake-utils.
8. Shell entry via direnv + nix-direnv (`.envrc`: `use flake`). This also creates the GC root that prevents "compiler path vanished after `nix-collect-garbage`".
9. `nix fmt` via treefmt-nix, wired into `checks.formatting`. CI runs `nix flake check`.

CMake:

10. `cmake_minimum_required` as low as the features require (3.28 covers presets v8, `FILE_SET`, `PROJECT_IS_TOP_LEVEL`). Do not pin to 4.x without need.
11. Never set `CMAKE_<LANG>_COMPILER` in `CMakeLists.txt`. It comes from the environment (`CC`/`CXX`, set by Nix), a toolchain file or a preset.
12. No global `include_directories`, `add_compile_options`, `link_libraries`, `CMAKE_CXX_FLAGS` edits. Use `target_*` with explicit `PUBLIC`/`PRIVATE`.
13. `target_compile_features(lib PUBLIC cxx_std_23)` instead of global `CMAKE_CXX_STANDARD`.
14. Optimisation and debug level come from `CMAKE_BUILD_TYPE` (preset), not hard-coded `-g -O1`.
15. Warnings and sanitizers behind options that default OFF for consumers and ON in dev presets and the Nix build; apply per target, `PRIVATE`.
16. Namespaced ALIAS (`proj::proj`), `FILE_SET HEADERS`, `install(TARGETS ... EXPORT)`, `install(EXPORT ... NAMESPACE proj::)`, `write_basic_package_version_file`, `configure_package_config_file`.
17. `include(CTest)` under `PROJECT_IS_TOP_LEVEL`; tests under `if(BUILD_TESTING)`; Catch2 via `find_package(Catch2 3)` + `catch_discover_tests`.
18. `CMakePresets.json` committed, `CMakeUserPresets.json` ignored; `binaryDir = build/<preset>`; `.clangd` → `CompilationDatabase: build/dev`; no symlink into the source tree.
19. Layout (Pitchfork): `include/<proj>/` public headers, `src/` sources + private headers, `apps/` executables, `tests/`, `cmake/` modules, `docs/` for prose and images, `nix/` for Nix. `build/` is reserved and ignored.

## 4. gsdb assessment

What gsdb already does well:

- Namespaced ALIAS `gsdb::libgsdb`; `target_include_directories` with `BUILD_INTERFACE`/`INSTALL_INTERFACE`; `target_compile_features(cxx_std_23)`; `install(TARGETS ... EXPORT)` + `install(EXPORT ... NAMESPACE gsdb::)`; private headers in `src/include`; Catch2 v3 with `catch_discover_tests`; test binaries located via a compile definition; out-of-source guard; `hardeningDisable = [ "fortify" ]` (correct and necessary for the `-O0` test targets under `-Werror`); GCC, Catch2, Zydis, libedit all from nixpkgs.

Deviations, ranked by impact, each with a fix:

| # | Where | Issue | Fix |
|---|-------|-------|-----|
| 1 | `flake.nix` | `nixpkgs.url = github:NixOS/nixpkgs/master`. Not a channel, not guaranteed cached; can trigger local toolchain builds. | Use `github:NixOS/nixpkgs/nixos-unstable`, then `nix flake update`. |
| 2 | `flake.nix` | No package derivation, so no `nix build`, `nix run`, `nix flake check`; shell deps are hand-listed and drift from the build. | Add `nix/package.nix` (zydis, libedit, catch2_3, pkg-config, cmake, ninja) and make the shell `mkShell { inputsFrom = [ gsdb ]; }`. |
| 3 | `flake.nix` shellHook | `exec zsh` replaces the shell process: `nix develop -c …` never runs its command, direnv cannot load the env, CI cannot use the shell. The `starship init` line after it is dead code. | Delete the hook. Add `.envrc` with `use flake` and enable nix-direnv; your own zsh and starship config apply unchanged. |
| 4 | `flake.nix` shellHook | `cmake -S . -B build >/dev/null 2>&1` on every entry hides configure errors and hard-codes the build dir. | Replace with `CMakePresets.json` and run `cmake --workflow --preset dev` explicitly. |
| 5 | No GC root for the dev shell | Today's failure: `nix-collect-garbage` deleted the gcc-wrapper that `build/CMakeCache.txt` pointed to. | nix-direnv (`.direnv/` holds the root) or `nix develop --profile .devshell`. |
| 6 | `flake.nix` | `CXXFLAGS = "-Wall -Wfatal-errors -std=c++23"` as an environment variable leaks into every compile in the shell, duplicates CMake, and diverges shell builds from `nix build`. | Remove. CMake owns flags. |
| 7 | `flake.nix` | Deprecated singular `devShell` output. | `devShells.<system>.default`. |
| 8 | `flake.nix` | flake-utils for one `eachSystem`; `i686-linux` listed but not buildable for this stack; unused `llvmPackages_18`, `boost`, `curl`, `gtest`, `cmakeCurses`. | `lib.genAttrs [ "x86_64-linux" "aarch64-linux" ]`; drop unused inputs. |
| 9 | `CMakeLists.txt` | `set(CMAKE_C_COMPILER gcc)` after `project()` is a no-op and the wrong place regardless. | Delete. Nix exports `CC`/`CXX`. |
| 10 | `CMakeLists.txt` | `cmake_minimum_required(VERSION 4.1.2)` excludes every CMake 3.x user for no feature you use. | `cmake_minimum_required(VERSION 3.28)`. |
| 11 | `CMakeLists.txt` | Global `add_compile_options(-Wall -Wfatal-errors -Wextra -Werror -g -O1)` hard-codes optimisation and debug level across build types and also hits the `-O0` test targets, relying on flag order. | Build type from presets (`Debug`, or `RelWithDebInfo`); warnings via a per-target function, `PRIVATE`. |
| 12 | `CMakeLists.txt` | Global `include_directories(${PROJECT_SOURCE_DIR})` is redundant with the target include dirs and leaks. | Remove. |
| 13 | `CMakeLists.txt` | `file(CREATE_LINK … compile_commands.json)` writes into the source tree at configure time. | Add `.clangd` with `CompileFlags: { CompilationDatabase: build/dev }` and delete the link logic. |
| 14 | `CMakeLists.txt` | `find_package(GTest REQUIRED)` + `include(GoogleTest)` unused; `ENABLE_TESTING` duplicates `BUILD_TESTING`; `ENABLE_INSTALL` and `gsdb_INSTALL_CMAKEDIR` unused (install rules hard-code the dir); `InstallRequiredSystemLibraries`, `CMakeDependentOption`, `CMakePackageConfigHelpers` included but unused; `GNUInstallDirs` included three times. | Prune to what is used; include `GNUInstallDirs` once at top. |
| 15 | `CMakeLists.txt` | `set(CMAKE_CXX_STANDARD 23)` globally and `cxx_std_23` on the target. | Keep the target feature; drop the global unless test targets need it. |
| 16 | `test/targets/CMakeLists.txt` | `-pie` passed as a compile option; PIE is a link property. `target_link_libraries(multi_threaded pthread)` uses a raw lib name with no visibility keyword. | `set_target_properties(t PROPERTIES POSITION_INDEPENDENT_CODE ON)`; `find_package(Threads)` + `target_link_libraries(multi_threaded PRIVATE Threads::Threads)`. Keep `-g -O0 -gdwarf-4`, those are intentional test inputs. |
| 17 | Layout | Images and prose (`*.jpg`, `linux_signals_and_interrupts.md`) at the root; `tools/` means dev scripts in Pitchfork while gsdb uses it for the CLI; `test/` vs `tests/`. | Move docs to `docs/`; optionally rename `tools/` → `apps/` and `test/` → `tests/`. Cosmetic. |
| 18 | Repo hygiene | No `CMakePresets.json`, `.clangd`, `.clang-tidy`, `.editorconfig`, CI, or `nix fmt`. | Copy them from the template. |

## 5. The template

Path: `~/projects/cpp-nix-template` (branch `main`). Verified today:
`nix flake check` (build + 2 Catch2 tests + treefmt check) passes; `nix build`
produces `bin/`, `lib/`, `include/`, `lib/cmake/myproject/`; inside
`nix develop`, the `dev`, `asan`, `tidy` and `release` workflow presets all
succeed and `clangd --check` is clean on every translation unit.

Contents and rationale are in its `README.md`. Key commands:

```bash
# bootstrap
nix flake init -t ~/projects/cpp-nix-template   # or git clone + rm -rf .git
./rename.sh <name> && git init && git add -A
direnv allow                                    # or: nix develop [--profile .devshell]

# daily
cmake --workflow --preset dev        # configure + build + test (Debug)
./build/dev/apps/<name>              # run the dev binary
ctest --preset dev                   # tests only
cmake --workflow --preset asan       # sanitizers
cmake --workflow --preset tidy       # clang-tidy
nix fmt                              # format C++, CMake, Nix
nix flake check                      # what CI runs
nix build && ./result/bin/<name>     # the Nix package
nix run . -- args                    # run without a checkout
```

## 6. Suggested migration for gsdb (proposal, not applied)

1. Add `.envrc` (`use flake`) and enable nix-direnv in the NixOS config. Fixes the GC-root problem immediately and independently of everything else.
2. Rewrite `flake.nix`: `nixos-unstable`, `lib.genAttrs`, `nix/package.nix` with `zydis libedit` in `buildInputs`, `pkg-config cmake ninja` in `nativeBuildInputs`, `catch2_3` in `checkInputs`, `doCheck = true`; shell via `inputsFrom` plus `clang-tools gdb cmake-format`, `hardeningDisable = [ "fortify" ]`. Keep the Zycore alias workaround in CMake.
3. Add `CMakePresets.json`. Because gsdb deliberately builds at `-g -O1`, give the `dev` preset `CMAKE_BUILD_TYPE=Debug` and `CMAKE_CXX_FLAGS_DEBUG=-g -O1` rather than hard-coding flags in `CMakeLists.txt`. The `-O0 -gdwarf-4` test targets keep their per-target options.
4. Apply items 9 to 16 above in `CMakeLists.txt`, `src/`, `tools/`, `test/`.
5. Add `.clangd`, delete the `compile_commands.json` link logic; update Neovim's on-save hook (or drop it, since `cmake --build` reconfigures automatically when CMake files change).
6. Update `CLAUDE.md` build commands to the preset workflow.
