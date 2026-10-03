# H-Exo-0-Jitter — operating rules for OpenCode

> **Why this file exists:** OpenCode V2 recognises **only `AGENTS.md`**. It does not
> fall back to `CLAUDE.md`, so the 690 lines of `CLAUDE.md` were not being loaded
> into any session. This file is a faithful, loadable summary of the rules that
> must never be violated. It is not a replacement.

---

## 1. Read the documentation FIRST

**Before writing a line of code, forming a hypothesis, or asserting any
architectural fact: find and read the authoritative documentation for the exact
register, opcode, field, ABI or protocol involved. Every time.**

Concretely, in this project that means, before touching anything:

| Question | Where the answer actually lives |
|---|---|
| a GIC/Mali/CPU register field | `third_party/trusted-firmware-a/`, `docs/vendor/panfrost/`, `u-boot/arch/arm/include/asm/` — all already vendored in-tree |
| a system-register encoding | Linux `arch/arm64/include/asm/sysreg.h` (fetch and read it) |
| what the project's firmware did before | `git log`, `git show`, `docs/rk3399/*` |
| how upstream Linux packs a descriptor | the vendored `linux_panfrost_*.c`, `docs/vendor/panfrost/mesa_v*.xml` |

**In-tree documentation beats the network. The network beats memory. Memory is not
a source.**

### Why this is rule 1

Every architectural error made on 2026-10-03 had the same cause: a field, opcode or
encoding taken from memory instead of from a document, and a conclusion built on it
before the field was checked.

| Claim taken from memory | What the document said | Cost |
|---|---|---|
| `ICC_SGI1R_EL1` at `S3_0_C12_C11_5` | correct by luck, wrong reasoning; field widths unverified | a false "we found an encoding bug" |
| `GICD_TYPER.IDbits` = bits[4:0] | it is **bits[19:24]**, real value **16** not 8 | a fabricated "Aff0 width = 0" root cause |
| `ICC_RPR_EL1` does not exist in GICv3 | it exists, `S3_0_C12_C11_3` | wrote it off as unimplemented |
| `GICR_WAKER.ChildrenAsleep=1` blocks delivery | it describes the PE's children, not interface delivery | a retracted hypothesis that had to be re-opened |
| `GICD_CTLR.DS` must be set | already set, `0x53` bit 6 | a whole A/B cycle proving a non-cause |
| invocation packing in `w8` | never read | still unresolved today |

Six errors, one cause. Every one was caught **after** a conclusion had been stated,
costing roughly a boot cycle and a chunk of the session each.

### What counts as having read it

Not: recalling a layout, or trusting a comment in our own source. A vendor header, a
spec XML, an upstream implementation, or the assembler itself. If the document and
the code disagree, the document wins and the code is the bug.

**If the documentation cannot be found, say so explicitly and mark the claim as
unverified.** Do not fill the gap from memory and do not build on it.

### The compiler is documentation too

When an encoding is disputed, assemble it and read the disassembly
(`aarch64-none-elf-objdump -d`). That is how `AP0R1..R3`/`AP1R1` were proven
*declared but not implemented* on this GIC — they trap, and no header says so.

## Mission

A forensic engineering audit of the H-Exo bare-metal RK3399 system. Reconstruct the
**actually implemented** architecture and execution path:

```
source → build → binary → boot → multicore startup → runtime evidence
```

**The binary beats filenames. Runtime beats documentation.**

The existence of a file is **not** evidence that it is active. Activity must be
established from: build references · includes · linker reachability · scripts that
actually invoke it · binary/string/disassembly evidence · UART logs · CI artifacts ·
documented results.

## Read-only by default

Never, during the audit:

- modify source files, Makefiles, linker scripts, boot or deployment scripts
- delete or rename files
- run destructive flash/write commands, or write to SD cards
- change Git history; reset, rebase or checkout branches
- overwrite binaries
- transmit anything to the NanoPi
- change U-Boot environment or network configuration

Allowed: read/search · inspect Git metadata · inspect build commands and existing
binaries · non-destructive static analysis · compiling or linking **in a temporary
directory only** · audit reports under a dedicated temporary/audit directory,
preferably outside the repository.

Do not optimize, refactor or rewrite project code during the audit.

## Do not bulk-read

`docs/mesa/` (a very large external Mesa tree) and `_nanopi_boot_src/lib/` are
imported, not H-Exo-owned. Read them selectively, never wholesale. The same
applies to `third_party/` — 10,125 files of vendored code.

## Evidence discipline

Every claim needs its source recorded. If a boot stage cannot be established from
evidence, it stays **`A72_FAIL_STAGE=UNDETERMINED`** — never promoted to a
conclusion because it reads better. See the `hexo-evidence` skill.

## Verifying C, assembly and Device Tree

Use the project's **own** toolchain, not a host compiler:

```sh
C:/gcc-arm/bin/aarch64-none-elf-gcc.exe -Wall -Wextra -fanalyzer -c FILE.c -o /tmp/x.o
```

Never `-fsyntax-only` with `-fanalyzer`: the analyser does not run and you get a
false all-clear. Note that `Makefile.ultra_optimized` builds with **no warning
flags at all**, so a clean build there proves nothing.

Full procedure, including the manual checks the compiler cannot do (shift UB,
alignment, MMIO volatility, DMA cache coherency, stack depth, linker load
addresses, `adrp`/`add` literal pools, Device Tree validation): see the
**`hexo-c-verify`** skill. Load it before touching `.c`, `.h`, `.s`, `.asm`,
`.dts` or `.lds`.

## Existing assets in this repository

| Asset | Purpose |
|---|---|
`hexo-auditor` agent | read-only forensic auditor, separate model, bash allowlist |
`/hexo-audit` command | starts the audit without modifying files |
`hexo-evidence` skill | converts logs and artifacts into a traceable evidence table |
`hexo-forensic-audit` skill | source → binary → boot evidence reconstruction |
`hexo-rk3399-boot` skill | RK3399 boot, EL transitions, PSCI, GICv3, MMU |
`hexo-c-verify` skill | C / assembly / Device Tree / linker verification |

## The one build command

```sh
pwsh -File tools/verify.ps1
```

That is the only build in this repository known to link. It compiles the canonical
source list (`tools/sources.json`), applies `-Werror=` per warning class, runs
`-fanalyzer` and `cppcheck`, links, and compares size + CRC32 + SHA256 against
`tools/baseline.json`. It exits non-zero on failure. Expect ~30 s.

**Do not use `make`.** Verified 2026-10-03 by actually running it:

| Makefile | Result |
|---|---|
| `Makefile.optimized` | retired; fails immediately via `$(error)`. `boot_optimized.s` never existed. Its recipes also call `wc`/`bc`/`rm`, absent on this host |
| `Makefile.neuro` | compiles 5 of 40 sources, then fails at link: undefined `cru_set_a72_freq_mhz`, `cru_get_last_pmic_diag`, `mmu_install_nc_region`, `cru_set_a53_freq_mhz`, `mali_power_on` |
| `Makefile.ultra_optimized` | builds with **no warning flags at all**, so a clean run proves nothing |
| `Makefile` | incomplete |

`.github/workflows/ci.yml` used to call `make -f Makefile.neuro`, i.e. the gate
was a gate that cannot pass. It now calls `tools/verify.ps1`.

Useful switches: `-Update` (rewrite the baseline — review the diff),
`-SkipAnalyzer` (fast iteration only, never for a ship decision), `-Log <file>`
(check a captured UART log against `docs/expected/markers.txt`).

### Why `-Wall` alone was not enough

Measured, not assumed. On the same source:

| flags | shift-negative-value | sign-compare | unused-parameter | array-bounds |
|---|---|---|---|---|
| `-Wall` | **missed** | **missed** | **missed** | caught |
| `-Wall -Wextra` | **missed** | caught | caught | caught |
| strict set in `verify.ps1` | **error** | warn | warn | **error** |

`INT_TO_FIXED(x) ((x) << 16)` was a live C11 6.5.7p4 undefined-behaviour bug in
~160 call sites, almost all passing negative literals, and the project's previous
build flags could not see it.

### Stack depth is a safety check, measured on the linked image

`-fstack-usage` reports one frame. What breaks bare metal is the sum of a chain,
and there is no MMU fault to catch an overflow here — it corrupts memory and the
symptom surfaces later as unrelated nonsense. `tools/stackchain.ps1` measures the
worst case on the image that actually ships:

```sh
pwsh -File tools/stackchain.ps1
```

Three things it gets right that a naive `-fstack-usage` reading does not:

| trap | what it would report | what is true |
|---|---|---|
| compare against the pool | 2144 of 32768 B, "fits easily" | `boot.s` gives each core **4096 B** (`sub x1, x1, x2` with `x2 = 0x1000*(core+1)`); the pool is not the budget |
| ignore exceptions | 1712 B | `vectors.s` branches to `exception_handler_irq` with **no SP switch**, so a handler stacks on the interrupted frame: worst case is the **sum** |
| ignore when IRQs turn on | reachable = naive | the deepest call is emitted *before* `msr daifclr, #2`, so it cannot receive an interrupt; the gate locates the unmask by address and only counts call sites after it |

Measured 2026-10-03 on `-O3 -march=armv8-a+fp+simd`, 40 sources, 308 nodes:

| | bytes |
|---|---|
| `kmain` frame | 1264 |
| deepest callee chain once IRQs are on (`hexo_offload_tick`) | 432 |
| exception chain on top (one of the four 256 B stubs, `exception_handler_irq`/`fiq`/`sync`/`serror` — they tie, so the reported name varies) | 432 |
| deepest root chain, any root (`bss_done`, no interrupt needed) | 1712 |
| **gated figure (max of the last two)** | **2128** |
| naive bound (ignores IRQ timing) | 2144 |
| per-core slot | 4096 |
| headroom | 1968 (48.0%) |

`kmain`'s own frame is 59% of it and is the only lever worth pulling. The second
largest frame, `chaos_memory_thrash` at 1024 B, is currently unreachable: the
disassembly contains **zero** branch sites to it, and zero to `chaos_apply`. If
chaos is ever wired up the figure becomes 1264 + 1024 + 432 = 2720 B, which still
fits but eats most of the margin.

Known limits, recorded so the number is not read as a proof: indirect calls are not
followed (there is exactly one `blr` today, the workqueue dispatch, and its target
is treated as its own root); a fault taken inside an interrupt handler is not
modelled (+432 B, and that still fits); it is static analysis of the linked image,
not runtime behaviour; and where two translation units define the same function
name the larger frame is taken, so `pipeit_sgi_hidden` at 144 B is an
over-estimate — the figure errs toward safe.

## Coding rules promoted from `.rules`

`.rules` and `.manifesto` sit in the repo root and are **not** `AGENTS.md`, so
OpenCode never loads them. Their actionable constraints now live here. The
manifesto's architecture prose stays in `.manifesto` as a design document; it is
not an agent rule and inlining 5 KB of it here would dilute this file.

- **No libc.** Never include `<stdio.h>`, `<stdlib.h>`, `<string.h>`. Only
  `<stdint.h>`, `<stddef.h>`, `<stdbool.h>`, `<stdarg.h>`.
- **MMIO only.** All hardware access through `volatile` pointers. Never cache an
  MMIO value in a non-volatile local across a possible write to that register.
- **No dynamic allocation** until our own heap exists. Static or stack only.
- **Stage 1 is pure AArch64 assembly**, in `.s`.
- **`panic()` contract**: dump X0–X30, PC, SP and EL to UART, then halt in a WFI
  loop. A deliberate fatal halt is fine; an accidental one is not — see the
  early-initialisation rule above.
- **UART2 base `0xFF1A0000`** is the RK3399 debug port.
- **Verify every MMIO address against the TRM** before reading it. A probe of an
  unmapped GIC address from U-Boot killed the board and needed a physical reset.

### Cache coherency between the A53 and A72 clusters

The clusters do not snoop each other. A value written by one core with a plain
store and read by another after `dc ivac` reads **stale memory**, which looks
exactly like "the other core never did it". This has produced two wrong
conclusions in this project already — once in `core/workqueue.c`, and again in a
diagnostic probe written the following day.

Any cross-core diagnostic must `dc civac` the slot on the writing side and
`dc ivac` it on the reading side.

## Read these before doing substantive work

- `CLAUDE.md` — full operating rules, 690 lines. **OpenCode will not load it for
  you; you must read it explicitly.**
- `H-Exo_MASTER_PROMPT.md` — master prompt for the audit.
- `H-Exo_MASTER_PLAN_v3.3.md` — current plan.
- `48H_PROGRESS.md` — current progress.

## Reporting

State what was verified and how (command plus result), and what remains
unverified. Do not present a hypothesis as a diagnosis. If the evidence does not
support a conclusion, say the stage is undetermined.