# H-Exo-0-Jitter — Project Roadmap & Achievement Log
## NanoPi M4 / RK3399 Bare-Metal Kernel

> Last updated: 2026-04-08
> Board: FriendlyElec NanoPi M4 · RK3399 · 2 GB DDR3 · Armbian BL31 v1.3 (2020-07-22)

---

## ✅ MILESTONE 1 — Bare-Metal Boot (COMPLETE)

**Goal**: Run bare-metal AArch64 code on RK3399, bypassing Linux entirely.

| Item | Status | Notes |
|------|--------|-------|
| Cross-compiler toolchain (gcc-arm-none-eabi 10.3) | ✅ | Bundled in `third_party/` |
| Linker script (`kernel_neuro.ld`) | ✅ | Kernel at 0x02080000, BSS/stack above |
| Boot entry (`boot.s` `_start`) | ✅ | EL2 identity-mapped, `.text.boot` section |
| UART2 early output (0xFF1A0000) | ✅ | 1500000 baud, no interrupt dependency |
| EL2 identity-map MMU | ✅ | 1 GB normal + device blocks, TTBR0_EL2 |
| Slab allocator (512 KB heap) | ✅ | Fixed-block, no dynamic malloc needed |
| Generic Timer (CNTPCT_EL0 @ 24 MHz) | ✅ | Used for all timing and telemetry |
| TFTP deploy pipeline | ✅ | `deploy_tftp_fixed.ps1` + TFTP server |
| Build system (`Makefile.neuro`, `build.bat`) | ✅ | Windows-native, no WSL needed |

---

## ✅ MILESTONE 2 — Hardware Subsystems (COMPLETE)

**Goal**: Bring up all critical RK3399 peripheral IP blocks.

| Subsystem | Address | Status | Notes |
|-----------|---------|--------|-------|
| CCI-500 Coherent Interconnect | 0xFFBB0000 | ✅ | Snoop + DVM enabled for A53 cluster |
| GICv3 Interrupt Controller | 0xFEE00000 / 0xFF010000 | ✅ | GICD + all 6 GICR redistributors pre-woken |
| GMAC Gigabit Ethernet | 0xFE300000 | ✅ | PHY reset, L2 beacon, RX IRQ on SPI 24 |
| Neural Weight Validation | — | ✅ | CRC32 check at build + runtime |
| Adaptive Scheduler (TinyML) | — | ✅ | 6→8→4 feedforward Q16.16 fixed-point |
| Telemetry engine | — | ✅ | Runtime metrics via Generic Timer |
| Heartbeat / Chaos subsystem | — | ✅ | EMA feedback, ACTIVE/THROTTLE hints |
| Work Queue | — | ✅ | Core-0 single-threaded baseline |

---

## ✅ MILESTONE 3 — SMP Bring-Up A53 Cluster (COMPLETE — 2026-04-08)

**Goal**: Bring all 4 Cortex-A53 cores (0–3) online and executing C code.

This was the hardest milestone. Full bug-hunt log below.

### 3.1 What Was Broken (History)

#### Stage A — PSCI SUCCESS but cores stuck `ON_PENDING`
- PSCI `CPU_ON` returned 0 but `AFFINITY_INFO` reported `ON_PENDING` indefinitely.
- **Root cause**: polling loop used `yield`-based busy-wait → timer resolution too coarse.
- **Fix**: replaced with `CNTPCT_EL0`-based 300 ms hardware timer poll.

#### Stage B — PSCI `ALREADY_ON` / wrong MPIDR encoding
- Some calls got `ALREADY_ON` error.
- **Root cause**: MPIDR passed to PSCI was missing `RES1` bit (bit 31 = `0x80000000`).
  RK3399 TF-A requires `hw_mpidr = 0x80000000 | (Aff1 << 8) | Aff0`.
- **Fix**: added `MPIDR_HW_MASK = 0x80000000ULL` in `core/smp.c`.

#### Stage C — Cores `ONLINE` in PSCI but NO kernel telemetry (beacon=0, no 'S' on UART)
- PSCI reported A53 cores 1–3 as `AFF_STATE_ON` after 1 poll.
- But: zero beacon writes, zero trace entries, no UART characters.
- **Hypothesis 1**: BL31 ERET to wrong address → trampoline at 0x00600000 test.
  - Result: BL31 WAS going to 0x00200000 (U-Boot's load address), not our trampoline.
- **Fix**: Deploy trampoline blob to `0x00200000` and use that as `entry_pa` for CPU_ON.

#### Stage D — Trampoline at 0x200000 executes, but branch to `secondary_entry` (0x02081000) crashes

Trampoline confirmed working:
- GRF `OS_REG2 = 0xBB` (canary written)
- UART `'X'` printed
- DRAM write `beacon[1] = 0xCAFEBABE` (at 0x02000008) — success
- But branch `br x6` to `0x02081000` → EL2 exception handler fires (GRF `OS_REG2 = 0xEE`)

**ESR_EL2 = 0x02000000** → `EC=0, IL=1` → on Cortex-A53 r0p4 this encodes
**SError (Asynchronous External Abort) from instruction fetch returning AXI SLVERR**.

#### Stage E — `msr daifset, #0xF` does NOT prevent the fault

Added SError masking (`PSTATE.A=1`) before the branch. Still crashed with EC=0.

**Root cause analysis**:
- Cortex-A53 TRM: "instruction fetch SLVERR is reported as **imprecise SError**"
- BUT with PSTATE.A=1 (masked), A53 places a **POISON instruction** in the pipeline instead of delivering SError
- POISON instruction generates a **synchronous EC=0 UNKNOWN fault** — fires regardless of DAIF
- So `daifset` cannot prevent it; the instruction fetch itself must succeed

#### Stage F — `SCTLR_EL2.I=1` (icache only) does NOT help

Tried enabling only the L1 instruction cache. Still crashed identically.

**Root cause**: With `M=0` (MMU off), AArch64 architecture defines memory as
`Normal Non-Cacheable` by default. Even with `I=1`, instruction fetch uses
**non-coherent AXI path** (no L2, no CCI). The AXI SLVERR protection applies
to this path regardless of icache enable.

#### Stage G — **ROOT CAUSE IDENTIFIED AND FIXED** ✅

**Real root cause**: Non-coherent AXI instruction fetch (both caches/MMU off on
secondary cores after BL31 ERET) hits an **AXI-level protection** at 0x02081000
that returns SLVERR. Core 0 doesn't hit this because it runs with MMU + caches
enabled (Normal Cacheable → coherent L2/CCI path → no SLVERR).

**Fix implemented** (`boot.s` trampoline, Step D):

```asm
// Load core 0's EL2 MMU registers from beacon[4..6] (stored by smp_init)
movz    x9,  #0x0200, lsl #16    // beacon = 0x02000000
ldr     x10, [x9, #32]           // TTBR0_EL2
ldr     x11, [x9, #40]           // TCR_EL2
ldr     x12, [x9, #48]           // MAIR_EL2
msr     ttbr0_el2, x10
msr     tcr_el2,   x11
msr     mair_el2,  x12
isb
// Enable MMU + D-cache + I-cache
mrs     x9,  sctlr_el2
orr     x9,  x9,  #(1 << 0)     // M=1: MMU
orr     x9,  x9,  #(1 << 2)     // C=1: D-cache
orr     x9,  x9,  #(1 << 12)    // I=1: I-cache
msr     sctlr_el2, x9
isb
msr     daifset, #0xF            // belt+suspenders
isb
// Now branch — fetch is Normal-Cacheable, coherent, no SLVERR
movz    x6, #0x0208, lsl #16
movk    x6, #0x1000
br      x6
```

**Fix in `core/smp.c`** (`smp_init`, before CPU_ON):

```c
// Store core 0's MMU config for secondary trampoline
u64 ttbr0, tcr, mair;
asm volatile("mrs %0, ttbr0_el2" : "=r"(ttbr0));
asm volatile("mrs %0, tcr_el2"   : "=r"(tcr));
asm volatile("mrs %0, mair_el2"  : "=r"(mair));
fb[4] = ttbr0;   // beacon[4] @ 0x02000020
fb[5] = tcr;     // beacon[5] @ 0x02000028
fb[6] = mair;    // beacon[6] @ 0x02000030
// dc civac + dsb sy to flush to DRAM before secondary reads
```

### 3.2 Final Proof (UART log 2026-04-08)

```
[SXSsmMCXSsmMCXSsmMCMP] CPU_ON core 3
  ↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑
  S=secondary_entry STEP1 beacon, s=STEP3 UART, m=STEP6 EL-check,
  M=STEP6 MMU-trace, C=STEP7 C-worker call — times 3 (cores 1,2,3)

[SMP] OK: 4 cores online
[SMP] beacon sentinel=0xBEEFDEAD EL=2 mpidr=0x80000003 → BEACON HIT
[SMP] GRF: R2=0xBB (no fault!) R3=0x3C0 (DAIF=all masked)
[SMP] entry_stage: C1=0x33 C2=0x33 C3=0x33  ← all in C-worker
[SMP] idle: C1=0xB9AFAB C2=0xB8F55E C3=0xB98F3A ← spinning alive
[SMP] psci C1/C2/C3 flags=0x19 path=c-worker
```

### 3.3 Technical Summary — Why It Works Now

| Layer | Core 0 (boot) | Secondary (before fix) | Secondary (after fix) |
|-------|--------------|----------------------|----------------------|
| MMU | M=1 (identity map) | M=0 (off after BL31) | M=1 (same TTBR0) |
| D-cache | C=1 | C=0 | C=1 |
| I-cache | I=1 | I=0 | I=1 |
| Instruction fetch path | L1→L2→CCI (coherent) | Direct AXI (non-coherent) | L1→L2→CCI (coherent) |
| 0x02081000 fetch result | OK | SLVERR → POISON → EC=0 | OK |

---

## 🔄 MILESTONE 4 — SMP Bring-Up A72 Cluster (IN PROGRESS)

**Goal**: Bring Cortex-A72 cores 4–5 online.

### Current State
- PSCI `CPU_ON` returns 0 (success) for both A72 cores
- `PMU_PWRDN_ST` changes from `0x3E` → `0x00` (all power domains powered up)
- But cores 4–5 remain `AFF_STATE_ON_PENDING` after 300 ms timeout
- No trampoline telemetry for cores 4–5

### Hypothesis
- A72 is a separate cluster (`Aff1=1`) with its own CCI-500 slave port
- The A72 cluster may require explicit CCI snoop enable for slave 0 (A53 uses slave 1)
- BL31 may need `SCU_B` (A72 SCU) to be enabled before secondaries can reach the trampoline
- MPIDR for A72: `0x80000100` (core 4), `0x80000101` (core 5) — different `Aff1`

### Known Facts
- `PMU_PWRDN_ST` bit 4 (A72 core 0) and bit 5 (A72 core 1) go to 0 → power domains came up
- But `AFFINITY_INFO` still returns `ON_PENDING`
- A72 warmboot path in TF-A may differ from A53 (different PMU domain: `CPU_SCU_B`)

### Next Actions
1. Enable CCI-500 slave 0 (A72 side) in addition to slave 1 (A53) in `hal/cci.c`
2. Check if A72 cluster needs separate RVBAR_EL3 setup in BL31
3. Add MPIDR A72 topology check in trampoline (Aff1=1 path)
4. Consider A72-specific power sequence: `CPUON_B` vs `CPUON_L`

---

## 🔄 MILESTONE 5 — Work Queue Multi-Core Dispatch (PLANNED)

**Goal**: Distribute work queue tasks across all online cores.

### Current State
- Work queue is single-threaded on core 0
- `smp_secondary_main()` loops on `wfe` (idle counter increments)
- No actual work dispatched to secondaries yet

### Plan
1. Add per-core task ring buffer (lock-free SPSC queue)
2. Core 0 enqueues tasks, secondary dequeues on `sev` wakeup
3. Add `smp_dispatch(core_id, fn, arg)` API
4. Benchmark: single-core vs 4-core neural inference throughput

---

## 🔄 MILESTONE 6 — Neural Inference Multi-Core Parallelism (PLANNED)

**Goal**: Parallelize TinyML feedforward network across A53 cluster.

### Plan
1. Split hidden layer (8 neurons) across 4 cores (2 per core)
2. Core 0 coordinates input/output, cores 1–3 compute partial dot products
3. Use shared memory + cache coherency (CCI-500 already enabled)
4. Target: 4× throughput reduction for inference latency

---

## 🔄 MILESTONE 7 — Network Stack Hardening (PLANNED)

**Goal**: Reliable IRQ-driven UDP/TCP stack.

### Current State
- GMAC RGMII PHY initialized
- L2 ARP + ICMP echo working (IRQ-driven via SPI 24)
- Basic Ethernet frame send/receive

### Plan
1. Add ARP table with timeout
2. Add UDP checksum validation
3. Add simple TFTP client (boot-time kernel reload without U-Boot)
4. Add telemetry export over UDP (syslog-compatible)

---

## 🔄 MILESTONE 8 — Deployment & CI Polish (PLANNED)

**Goal**: Reliable one-command deploy cycle.

### Known Issues (Fixed This Session)
| Bug | Fix |
|-----|-----|
| `deploy_tftp_fixed.ps1` false-positive DRAM fail detection | Pattern `"channel init fail"` narrowed; removed blocking `Read-Host` |
| PSCI SYSTEM_RESET leaves DDR controller dirty → next boot fails | User must do physical power cycle after soft reset |
| `COM3` access denied after orphaned PowerShell processes | `Stop-Process` cleanup before deploy |

### Remaining Issues
1. After `PSCI SYSTEM_RESET`, board needs physical power cycle (DDR dirty state)
   - **Option A**: kernel reboot writes `CRU_GLB_SRST_FST = 0xFDB9` (hardware reset)
   - **Option B**: CI pipeline always forces power cycle before flash
2. TFTP occasionally times out on first PHY autonegotiation — retry logic in place but could be faster

---

## 📐 Architecture Decision Log

### ADL-001 — Trampoline at 0x00200000
- **Decision**: Deploy SMP relay trampoline to 0x00200000, not to kernel text.
- **Reason**: BL31's `cpuson_entry_point` → 0x200000 is what BL31 actually uses for
  secondary ERET (it ERETed to 0x200000 for U-Boot previously). Using any other address
  requires BL31 to honour our CPU_ON `entry` argument, which Armbian BL31 v1.3 does
  only after secure on_finish completes.
- **Date**: 2026-04

### ADL-002 — Secondary MMU Enable in Trampoline
- **Decision**: Trampoline enables secondary EL2 MMU (M+C+I) by reusing core 0's
  `TTBR0_EL2 / TCR_EL2 / MAIR_EL2`, stored in `beacon[4..6]` before CPU_ON.
- **Reason**: Without MMU, secondary instruction fetch from `0x02081000` goes via
  non-coherent AXI, returns SLVERR, A53 inserts POISON instruction → EC=0 sync fault.
  With M=1 (identity map) the fetch is Normal-Cacheable coherent — no SLVERR.
- **Alternative rejected**: Copy `secondary_entry` code to 0x201000 (low DRAM) —
  would require rewriting all `adrp/bl` instructions as `movz/movk/blr` (PI form)
  AND still couldn't reach C code at 0x0208xxxx without MMU.
- **Date**: 2026-04-08 ← **PROVED WORKING**

### ADL-003 — MPIDR Encoding with RES1 Bit
- **Decision**: Always pass `hw_mpidr = 0x80000000 | raw_mpidr` to PSCI CPU_ON.
- **Reason**: RK3399 TF-A validates `MPIDR_EL1[31]` (RES1 bit). Without it, PSCI
  returns `PSCI_E_INVALID_PARAMS`.
- **Date**: 2026-03

### ADL-004 — No BL31 RAM Override
- **Decision**: Do NOT copy custom BL31 binary to 0x40000 via U-Boot `cp.b`.
- **Reason**: `cpuson_flags` and `cpuson_entry_point` arrays reside at the same
  physical address range (0x40000). Overwriting them corrupts PSCI state, causing
  secondary cores to hang in BL31 wfe loop forever.
- **Date**: 2026-03

---

## 🔬 Diagnostic Infrastructure

The following telemetry is permanently wired and survives across SMP failures:

| Signal | Location | What It Proves |
|--------|----------|----------------|
| `PMUGRF_OS_REG1` (0xFF320304) | GRF MMIO | Secondary Aff0 \| 0xA0 → trampoline ran |
| `PMUGRF_OS_REG2` (0xFF320308) | GRF MMIO | 0xBB=reached, 0xEE=faulted in EL2 handler |
| `PMUGRF_OS_REG3` (0xFF32030C) | GRF MMIO | DAIF before branch (or ESR_EL2 on fault) |
| `beacon[0]` (0x02000000) | DRAM | core_idx written by secondary_entry |
| `beacon[1]` (0x02000008) | DRAM | 0xBEEFDEAD = secondary_entry reached; 0xCAFEBABE = trampoline DRAM write |
| `beacon[2]` (0x02000010) | DRAM | CurrentEL at entry |
| `beacon[3]` (0x02000018) | DRAM | raw MPIDR_EL1 |
| `beacon[4..6]` (0x02000020+) | DRAM | core 0's TTBR0/TCR/MAIR (for secondary MMU) |
| `smp_trace_page` | BSS | per-core bitmasks for each bring-up stage |
| `smp_entry_stage[]` | BSS | 0x11=asm entry, 0x22=mmu+stack, 0x33=C-worker |
| `smp_idle_counters[]` | BSS | spinning counter, proves core is alive |
| EL2 vector @ 0x200800 | Trampoline | catches any fault before secondary_entry |
| UART blast `'X','S','s','m','M','C'` | UART | character-level progress markers |

---

## 📅 Changelog

| Date | Event |
|------|-------|
| 2026-03 | Project bootstrapped, core 0 bare-metal boot working |
| 2026-03 | GICv3, CCI-500, GMAC, TinyML engine online |
| 2026-03 | PSCI CPU_ON implemented, cores stuck ON_PENDING |
| 2026-03 | MPIDR RES1 fix → PSCI accepts calls correctly |
| 2026-03 | Timer-based polling → cores report ONLINE via PSCI |
| 2026-03 | Trampoline at 0x200000 → BL31 ERET target confirmed |
| 2026-04 | PMU GRF diagnostic wired (OS_REG 1/2/3) |
| 2026-04 | EL2 exception vector in trampoline → ESR capture |
| 2026-04 | Root cause: non-coherent AXI SLVERR for instruction fetch |
| 2026-04 | `daifset` and `SCTLR.I` tried and confirmed insufficient |
| **2026-04-08** | **MMU enable in trampoline → A53 cores 1,2,3 ONLINE, all in C-worker** |
| 2026-04-11 | GitHub Action **RK3399 BL31 + trust.img**: TF-A v2.14 BL31 + rkbin `trust_merger` artifact (replace Windows packer) |
| 2026-04-11 | TF-A RK3399: **PMUSRAM_RSIZE 8→16 KiB** in CI (upstream link overflow ~3.9 KiB; patch file in `patches/`) |
