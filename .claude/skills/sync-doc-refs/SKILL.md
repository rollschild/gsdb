---
name: sync-doc-refs
description: Re-sync the explanation/walkthrough docs in .claude/ and .codex/ with the current source tree — fix stale file:line references, refresh quoted code excerpts, and correct signatures, member names, and behavior claims that the code has since outgrown. Use when asked to "update the docs", "sync doc line numbers", "check the doc references", "are the docs still accurate", or as a periodic doc-maintenance pass after refactoring.
---

# Sync explanation docs with the source tree

The docs under `.claude/` and `.codex/` are line-anchored walkthroughs of gsdb's
internals. Every edit to `src/`, `include/`, `tools/`, or `test/` silently rots
them: line numbers slide, signatures change, and "current implementation" claims
become history. This skill re-anchors them.

## Hard rules

1. **Never edit source files.** `src/`, `include/`, `tools/`, `test/`,
   `CMakeLists.txt`, `flake.nix` are strictly read-only here — per `CLAUDE.md`.
   If a doc turns out to be *right* and the code *wrong*, report it; do not fix
   the code.
2. **Never touch `$HOME`.** Everything stays inside the repo.
3. **Only `.md` files get edited**, and only within the requested scope.
4. **Minimal diffs.** Change the numbers, the excerpt, and the sentences that
   are now false. Do not rewrite prose that is still correct, do not "improve"
   the author's voice, do not reformat, do not reorder sections.
5. **Never delete an explanation just because its subject moved.** Re-anchor it.
   Only propose deletion when the described code is genuinely gone, and say so
   in the report rather than doing it silently.

## Arguments

| Arg | Effect |
| --- | --- |
| *(none)* | Full default scope: all tracked `*.md` except `CLAUDE.md` and `build/**` |
| `<path>…` | Restrict to the named docs (globs fine) |
| `--check` | Report only, make no edits |
| `--harden` | While editing a reference anyway, append its symbol name (see §6) |

`CLAUDE.md` is out of scope by default — it carries architectural prose, not
line anchors, and `/init` maintains it. Include it only if explicitly asked.

## Step 1 — Scan

```bash
.claude/skills/sync-doc-refs/scripts/scan-refs.sh            # full scope
.claude/skills/sync-doc-refs/scripts/scan-refs.sh .claude/stack-unwinding.md
```

For every `path:line` reference in every doc the script prints the resolved
repo file, the file's current length, **the source text now sitting at that
line**, and whether the file has been edited since the doc was last committed.

Status legend:

| Flag | Meaning | Action |
| --- | --- | --- |
| `OK` | File exists, line in range | Read the printed text — does it still match what the doc claims is there? |
| `RANGE` | Line past EOF | Definitely stale, re-anchor |
| `NOFILE` | No such repo file | Usually an illustrative example (`demo.cpp`, `main.cpp`) or an external path (a Nix store `.cmake` file) — **leave those alone**. Only act if it names a file the project really once had. |
| `AMBIG` | Bare name matches several repo files | Resolve from the doc's context |
| `MOVED` | Source edited since the doc's last commit | Treat every reference into that file as suspect, even the `OK` ones |
| `PROSE` | Bare "line 99" with no filename | Resolve against the doc's surrounding context (§4) |

The script also prints the doc's last commit and the `git diff` command to see
everything that changed in a source file since the doc was written. That diff is
the single most useful input for this whole job.

## Step 2 — Prioritize

Work doc by doc, starting with the ones whose referenced files are `MOVED`.
A doc with zero `MOVED` files and no `RANGE`/`AMBIG` hits usually needs nothing
beyond a spot-check of its code excerpts.

## Step 3 — Re-anchor by symbol, never by delta

**Do not compute a line offset and add it to every reference.** Code moves
unevenly. Re-locate each anchor by what it *is*:

```bash
grep -n 'execute_unwind_rules' src/dwarf.cpp          # where did it go?
grep -n 'case DW_CFA_offset:' src/dwarf.cpp           # for an anchor mid-function
git log -L 809,851:src/dwarf.cpp                      # trace one region's history
git diff <doc-commit> HEAD -- src/dwarf.cpp           # everything that changed
```

Read the doc sentence around the reference first — it tells you what the anchor
is supposed to point at ("`execute_unwind_rules` (`src/dwarf.cpp:809`)"). Find
that thing, use its current line.

Conventions to preserve when rewriting a range:

- A reference to a **function** points at its first declaration line (the return
  type / signature line), not its opening brace or first statement.
- A **range** `start-end` spans the whole construct the prose discusses —
  recompute *both* ends; the end line drifts more than the start.
- Keep the doc's existing citation style. Some docs use `src/target.cpp:76`,
  others use bare `target.cpp:170-178`. Match the surrounding text; don't
  normalize across a doc.

## Step 4 — Bare prose references ("line 99")

These have no filename, so they resolve against the doc's local context: either
the fenced excerpt printed just above, or the file named in the enclosing
section heading. Determine which, then:

- If they index into a **quoted excerpt** in the doc, they are only valid if
  that excerpt still matches the source — refresh the excerpt (§5) and renumber
  to the excerpt's *new* real line numbers.
- If they index into the **live file**, re-anchor as in §3.
- If they index into an illustrative example the doc invented, leave them.

## Step 5 — Refresh quoted code

Fenced blocks are the second rot source. For each block that looks like verbatim
project source (not pseudocode, not disassembly, not shell):

1. Take a distinctive line from it and `grep -n` for it in the repo.
2. If it is not found verbatim, the excerpt is stale — replace it with the
   current text, preserving the doc's presentation (its truncation with `...`,
   its `// comment` annotations, its highlighting of the line under discussion).
3. If the *shape* changed (a parameter added, a `std::vector` became a `span`,
   a member renamed), fix every mention of it in the prose too, not just the
   block. Grep the doc for the old identifier.

Blocks that are deliberately schematic — ASCII diagrams, DWARF byte layouts,
sample REPL sessions, hypothetical `demo.cpp` — are not excerpts. Leave them
unless the behavior they illustrate actually changed.

## Step 6 — Correct stale claims, not just numbers

This is the part a line-number pass misses. For each doc, check that its
assertions still hold:

- **Signatures and types** — parameter lists, defaults, return types,
  `const`/reference qualifiers, template parameters.
- **Names** — functions, classes, members, enumerators, section names, CMake
  targets, test tags.
- **Behavior** — "returns `nullptr` when…", "throws `error::send`", "uses
  `emplace`, which does not overwrite", "not yet implemented".
- **Caveat / "Notes and caveats" sections** — these list known bugs. If a bug
  has since been fixed, say so and remove or rewrite the item; a doc warning
  about a fixed bug is worse than no warning. Verify against the code, not
  against the git log message.
- **Cross-references between docs** — a `[[doc]]` or relative link whose target
  was renamed.

With `--harden`, when you are already editing a reference, extend it with the
symbol it points at — `` `src/dwarf.cpp:809` `` becomes
`` `src/dwarf.cpp:809` (`execute_unwind_rules`) `` — so the next run can
re-anchor mechanically even if the number is wrong. Do not do this to
references you are not otherwise touching.

## Step 7 — Verify

Re-run the scanner over the docs you edited. Every reference should come back
`OK`, and the printed source text should be the thing the prose describes.
`MOVED` will still appear (the doc is not committed yet) — that is expected;
what matters is that the printed line matches the claim.

Spot-check by reading two or three of the updated passages against the actual
source. The scanner proves a line *exists*; only reading proves it is the
*right* line.

## Step 8 — Report

Do not just say "updated the docs". Report, grouped by doc:

- references re-anchored (old → new), with the reason if it was not a pure move;
- code excerpts refreshed;
- prose claims corrected, quoting the old claim and the new one;
- **anything you could not resolve** — a symbol that no longer exists, an
  ambiguous anchor, a doc describing a design that has been replaced. These need
  a human decision; list them explicitly rather than guessing.
- **source bugs noticed while reading.** You are reading code and docs side by
  side, which is when discrepancies surface. Report them; do not fix them.

Leave the changes uncommitted unless asked to commit.
