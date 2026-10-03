# tools/bin

Portable third-party binaries. Nothing here is committed; see `.gitignore` in this
directory for how `cppcheck` was produced and how to reproduce it.

`tools/verify.ps1` looks for `cppcheck.exe` here (or `tools/bin/cppcheck` on a
non-Windows host) and degrades honestly when it is missing: it prints a WARN and
names the defect classes that only cppcheck covers, rather than passing silently.

Why cppcheck is not optional in principle: GCC's `-fanalyzer` provably does not
look for buffer overflows, format-string defects or object-lifetime problems. The
strict `-Werror=` set catches shift-negative-value, array-bounds,
maybe-uninitialized, return-type and implicit-function-declaration, but nothing in
the toolchain covers the lifetime and format-string classes. cppcheck does.

## Measured coverage on this tree

cppcheck 2.22.0 parses **34 of 37** C files. It cannot parse the other three
because they use the GCC-specific global register variable declaration
`register u64 x0 asm("x0")`:

- `core/smp.c`
- `neuro/pipeit.c`
- `main_neuro.c`

Those three therefore get **no** static analysis from cppcheck at all. A clean
cppcheck run does not mean full coverage, and `verify.ps1` prints that fact on
every run so it cannot be forgotten.

## Current findings

5 gated findings, all baselined as verified false positives in
`tools/cppcheck-baseline.txt`, plus 88 advisory findings that are not gated
(`constVariablePointer`, `variableScope`, `unusedStructMember`, `badBitmaskCheck`,
`knownConditionTrueFalse`, `unreadVariable`). The advisory set is dominated by
cosmetic const-correctness and dead-condition notes.
