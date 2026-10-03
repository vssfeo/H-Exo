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

## ✅ MILESTONE 4 — SMP Bring-Up A72 Cluster (COMPLETE — 2026-04-22)

**Goal**: Bring Cortex-A72 cores 4–5 online.

### 4.1 What Was Broken (History)

#### Stage A — A72 cores reset entire SoC after CPU_ON
- PSCI `CPU_ON(0x80000100, 0x200000)` returned success but board immediately reset.
- Reset cause: `RST` (SoC-level watchdog/SError).
- No trampoline telemetry survived the reset.

#### Stage B — GRF OS_REG2 beacons survive reset
- Added step-by-step beacons (`0xB1..0xB5` in trampoline, `0xC1..0xC4` around CPU_ON).
- Beacons showed crash happened **inside the SMC call** — no post-CPU_ON beacon written.

#### Stage C — CCI-500 CHANGE_PENDING stuck for both clusters
- CCI state log: `a53=0x80000002(S=0,D=1) a72=0x80000002(S=0,D=1)`
- Bit 31 = CHANGE_PENDING set, snoop not enabled.
- Our `cci500_enable()` wrote `0x3` to CCI slave registers from **Non-Secure EL2**.
- These NS writes created CHANGE_PENDING transactions that **never complete** —
  CCI-500 is managed by TF-A Secure world, NS writes are ignored but leave PENDING stuck.

#### Stage D — Deploy script disabled caches before kernel launch
- `deploy_tftp_fixed.ps1` ran `dcache off` / `icache off` before `go 0x02080000`.
- This broke cache coherency for A72 cores that need coherent CCI path.

#### Stage E — ROOT CAUSE IDENTIFIED AND FIXED ✅

**Root cause**: Two independent problems:

1. **NS CCI writes** — `cci500_enable()` and PMU CCI500_CON writes from NS EL2
   created stuck CHANGE_PENDING in CCI-500. When TF-A's `cci_enable_snoop_dvm_reqs()`
   ran during `pwr_domain_on_finish`, it spun forever on CHANGE_PENDING → watchdog reset.

2. **Cache disable** — `dcache off`/`icache off` in deploy script broke coherency.

**Fixes applied**:
1. `hal/cci.c`: `cci500_enable()` → **no-op** (TF-A manages CCI from Secure)
2. `core/smp.c`: Removed PMU CCI500_CON write (same NS interference)
3. `deploy_tftp_fixed.ps1`: Removed `dcache off` / `icache off`
4. `core/smp.c`: Added `dc civac` + `ic ialluis` + `dsb/isb` before CPU_ON (cache flush only)

### 4.2 Final Proof (UART log 2026-04-22)

```
[SMP][CCI] post-cpu_on a53=0xC0000003(S=1,D=1) a72=0xC0000003(S=1,D=1)
[SMP] core 4 ONLINE after 1 polls
[SMP] core 5 ONLINE after 1 polls
[OK] SMP: 6 cores online
```

CCI shows `0xC0000003` = snoop+DVM enabled, no PENDING — TF-A did its job.

### 4.3 Technical Summary

| Layer | Before fix | After fix |
|-------|-----------|----------|
| CCI-500 snoop | NS write → stuck PENDING | No-op, TF-A manages |
| PMU CCI500_CON | NS write → interference | Removed |
| Deploy caches | dcache/icache off | Kept enabled |
| Cache flush before CPU_ON | None | dc civac + ic ialluis |
| A72 bring-up result | SoC reset | All 6 cores online |

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

### ADL-005 — No NS CCI-500 Register Writes
- **Decision**: Never write CCI-500 slave interface or PMU_CCI500_CON registers from Non-Secure EL2.
- **Reason**: NS writes to CCI Snoop Control Registers create CHANGE_PENDING transactions that never complete (CCI is managed by TF-A Secure world). The stuck PENDING blocks TF-A's `cci_enable_snoop_dvm_reqs()` during `pwr_domain_on_finish`, causing infinite wait → watchdog → SoC reset.
- **Evidence**: `a53=0x80000002(S=0,D=1) a72=0x80000002(S=0,D=1)` — PENDING stuck for both clusters. After removing NS writes: `a53=0xC0000003(S=1,D=1) a72=0xC0000003(S=1,D=1)` — TF-A enables snoop correctly.
- **Date**: 2026-04-22

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
| 2026-04-22 | GRF OS_REG2 beacons in trampoline + smp.c — survive SoC reset, pinpoint crash inside SMC |
| 2026-04-22 | **Root cause: NS CCI-500 writes → stuck CHANGE_PENDING → TF-A deadlock → watchdog reset** |
| 2026-04-22 | `cci500_enable()` → no-op, PMU CCI500_CON write removed, deploy cache disable removed |
| **2026-04-22** | **A72 cores 4,5 ONLINE — all 6 cores (4×A53 + 2×A72) operational** |
| 2026-10-02 | A72 cores 4,5 come up ONLINE and execute code, but never take an interrupt - defect recorded here for the first time || 2026-10-02 | `wq_dispatch()` had an unbounded `while (!s->done)` - it wedged the initiator itself, which is why every log stopped at `[BASELINE_A72] Dispatching`. Now bounded to 5 s |
| 2026-10-03 | `GICD_CTLR.DS=1` (the RK3399 insecure-integration quirk Linux applies) tested and **refuted**: `c4 irq_cnt 0x0 -> 0x0` with `gicd_ctlr=0x53` |
| 2026-10-03 | Full investigation log written to `docs/rk3399/A72_INVESTIGATION_LOG.md` |
| **2026-10-03** | **A72 INTERRUPT ROOT CAUSE FOUND AND FIXED: H-Exo's own `gicv3_prewake_redistributors()` wrote `GICR_WAKER.PS=0` on frames 4/5 before CPU_ON, wedging the RD wake transition (`CA=1` forever, PS writes ignored from NS and EL3, BL31 `mark_core_awake` no-ops at PS=0). Prewake removed for A72, all NS/EL3 WAKER writes to frames 4/5 deleted. Measured: `SGI_TEST c4 irq_cnt 0x0->0x1`, boundary B1 PASS on all cores, core 4 handled 292 pipeline SGIs. SGI pipeline mode stays OFF (chain hidden->output->done incomplete: 317/1000, output/done sends=0); polling mode ships (1000/1000 @ ~10.9 µs)** |
| **2026-10-03** | **Pipeline load test (`[LOADTEST]`, 10000 frames, 0 timeouts, tag-validated cross-core timestamps).** The long-quoted "10.6-10.9 us/frame" was a measurement artefact: UART progress prints inside the timed window (~6 ms of 10.6 ms) plus cold start at N=1000. True steady state, polling mode: **4.39 us/frame wall, 3.25 us accounted**. Per phase (avg): P1 submit 1291 ns (two `dc cvac` + `dsb sy` to PoC - the dominant sync cost), P2 wake 41 ns (48.8% of frames are picked up BEFORE submit even returns - the spin loop is that tight), P3 fused compute 1208 ns, P4 completion 166 ns, P5 producer detects bell 583 ns. Single-core A72 runs the whole net in ~200 ns, so for THIS network the distributed closed-loop path is 16.6x slower than one core - distribution only pays for bigger payloads or open-loop streaming. Next lever, quantified: frame slots into the NC region removes the cvac/dsb from P1 (P4 proves the same work costs 166 ns NC) |

| 2026-10-02 | `CPUECTLR_EL1.SMPEN` measured = 1 on both A72 via a new SiP SMC; SMPEN hypothesis eliminated (see ADL-006) |
| 2026-10-02 | RK_SIP_GICR_WAKE_TRY driven from EL3: status flag 0x2 timeout; ChildrenAsleep is normal on working A53s too |
| 2026-10-02 | `A72_PROBE_B` verdict bug fixed - it printed HPPIR read before injection, so it proved nothing |
| 2026-10-02 | ADB400 big-cluster to GIC handshake dumped at 4 stages: identical to the little cluster, hypothesis eliminated |
| 2026-10-02 | BL31 built by CI and written to the SD card at LBA 0x4000 over U-Boot `mmc write` - no card removal required |
| 2026-10-02 | The BL31 GICR wake patch measured to be a no-op; relabelled from `fix` to a labelled experiment || 2026-10-02 | `L2ACTLR_EL1` probed on cores 4,5 via a new SiP SMC: `raw=0x10` on both, no control bit set - hypothesis not supported |
| 2026-10-02 | Documented that TF-A's two `ChildrenAsleep` poll loops are unbounded - any future WAKER write could hang BL31 in EL3 forever |
| 2026-10-02 | Corrected the `Quiesce` row: the SGI frame has no `GICR_CTLR` at all, per IHI 0069 Table 8-29 |
| 2026-10-02 | Project docs reviewed: `PHASE2_MALI_PIPEIT_RECOVERY.md` credited 928/1000 SGI *sends* as 92.8% *delivery*; real delivery was 0, the polling fallback caught all 1000 |
| 2026-10-02 | `MASTER_PLAN_v3.2` classified the A72 interrupt failure as silicon errata and rebuilt IPC around a SEV fallback on that basis - premise now measured false, ADL-008 needs superseding || 2026-10-02 | **CONTROL EXPERIMENT**: Armbian Linux on the same card with the same BL31 brings up 6/6 CPUs; cores 4,5 take thousands of interrupts. Hardware and BL31 exonerated |
| 2026-10-02 | **CORRECTION**: the earlier conclusion `fault is below the software interface, internal to the A72 cluster` is withdrawn - it was inferred from probes running inside H-Exo, which configures the GIC itself |
| 2026-10-02 | New prime suspect: `gicv3_init()` runs after `smp_init()` and begins with `gicd_write(GICD_CTLR, 0)`, disabling the Distributor after the CPU interfaces were programmed |



---

> **Full attempt-by-attempt record:** [`A72_INVESTIGATION_LOG.md`](A72_INVESTIGATION_LOG.md)
> - every hypothesis with its evidence, every real defect found and fixed, the
> leads that did not pan out, and the process mistakes made along the way.

## A72 Interrupt Delivery - Investigation (2026-10-02)

**Status: OPEN, but the fault is NOT below the software interface. A control
experiment on 2026-10-02 exonerated BL31 and the hardware; the fault is in
H-Exo's own GIC bring-up. Earlier revisions of this section said the opposite
and were wrong.**

> **Correction (2026-10-02).** An earlier version of this entry concluded that the
> redistributor-to-CPU-interface path inside the A72 cluster was dead. That
> conclusion was reached from measurements taken *inside H-Exo*, and H-Exo is what
> configures the GIC. It is therefore not admissible as evidence about the
> hardware. Armbian Linux, booted from the same card with the **same BL31 binary**,
> brings up all six cores and cores 4 and 5 receive thousands of interrupts from
> the same GICv3 distributor. Hardware and BL31 are exonerated.

The 2026-04-22 entry below says "all 6 cores operational". That is true only for
*code execution*. The A72 cores power on, reach C, run the baseline benchmark
and are reported ONLINE by PSCI - but they **never receive an interrupt**, so
they never join the dispatch rotation. That defect was not recorded until now.

### Measured facts (NanoPi M4, RK3399, 2 GiB DDR3, TF-A v2.14.0)

| Observation | Value |
|---|---|
| A72 executes code | `MPIDR_EL1=0x100`, `cpu_mhz_actual=1607`, baseline `avg_cycles=0xE4` |
| A72 reaches the C worker | `entry_stage C4=C5=0x33`, `A72_STAGE=COMPLETED entered_c=1 online=1` |
| Dispatch rotation | cores 1/2/3 only (233/233/233 over 70 s); cores 4/5 never appear |
| SGI to core 1 | `ispendr0 0x0 -> 0x2 -> 0x0`, `irq_cnt 0 -> 1` - consumed |
| SGI to core 4 | `ispendr0 0x0 -> 0x2 -> 0x2`, `irq_cnt 0 -> 0` - latches forever |
| **PPI injected into core 4's OWN redistributor** | **`ICC_HPPIR1_EL1` stays `0x3FF`; never reaches its OWN CPU interface** |
| `GICR_WAKER` from U-Boot, all 6 frames | core0 `0x00`, core1..5 `0x06` |
| `GICR_WAKER` in H-Exo after prewake | core1..5 `0x04` |
| `CPUECTLR_EL1.SMPEN` on cores 4/5 | `1`, read from EL3 via `RK_SIP_SMPEN_GET` |
| `RK_SIP_GICR_WAKE_TRY` driven from EL3 | `status_flags=0x2` (timeout), `before=0x4 after=0x4` |
| CPU-interface regs, core 1 vs core 4 | identical: `sre=0xF pmr=0xF8 igrpen1=1 isen=0x2000FFFF igrp=0xFFFFFFFF` |
| `GICR_CTLR`, RD and SGI frames | `0x0` on all six cores |
| ADB400 big<->GIC across 4 stages | no requests ever asserted; all six `CLR_*_HW_ST` set, same as the little cluster |
| CCI-500 snoop node | A53 `S=1,D=1` throughout; A72 `S=0,D=0` until `poll-end` |
| **CONTROL: Linux on the same card, same BL31** | **6/6 processors online** |
| **CONTROL: interrupts seen by each CPU under Linux** | IPI1 `3719 4663 3420 4291 4338 4610`; arch_timer `11487 7877 1873 1651 3131 2953`; rk_timer `933 779 738 663 647 675` - **cores 4 and 5 take interrupts in thousands** |
| **CONTROL: Linux GIC init note** | `GIC: enabling workaround for GICv3: Insecure RK3399 integration`, `GICv3: Broken GIC integration, security disabled` |

### Hypotheses tested and eliminated

| Hypothesis | Why eliminated |
|---|---|
| `ChildrenAsleep=1` blocks delivery | **REVERSED 2026-10-03 - root cause.** The A53 comparison value was measured while the cores were off; at delivery time working A53 frames read `0x0` and dead A72 frames `0x4`. Wedged by H-Exo's own prewake writing PS=0 before CPU_ON; fix = stop writing A72 frames, BL31's handshake then completes. See A72_INVESTIGATION_LOG.md §Resolution |
| `CPUECTLR_EL1.SMPEN` never set | Measured `=1` on both A72 cores |
| GICR_WAKER handshake is a software sequencing bug | `RK_SIP_GICR_WAKE_TRY` drives the full two-phase handshake from EL3 and still times out |
| Redistributor register state differs | RD and SGI frames are identical between a working A53 and a dead A72 apart from the TYPER CPU_Number field |
| `GICR_CTLR` is never written | Measured `0x0` on all six cores, including the working ones |
| CCI-500 snoop disabled for A72 during bring-up | A real TF-A ordering defect: `plat_cci_enable()` uses `read_mpidr()`, so a cluster node can only be enabled by a PE inside that cluster. But IHI 0069 has zero occurrences of "snoop" - GIC MMIO is Device memory with in-order arrival and the Redistributor to CPU interface link is AXI4-Stream packets. No causal path to SGI delivery |
| ADB400 big-cluster to GIC handshake broken | Measured identical to the little cluster at all four sampled stages |
| A stalled `Quiesce` on the SGI frame | There is no such register. Per IHI 0069 Table 8-29 the SGI frame register map starts at `0x0080` (`GICR_IGROUPR0`); `GICR_CTLR` exists only in the RD frame, and `Quiesce` is a GIC Stream Protocol *command*, not a bit. Reading `SGI_base + 0x0000` returns zeros because nothing is mapped there |
| A72 `L2ACTLR_EL1` has the L2 GIC-timer clock forced off | Measured `raw=0x10` on both cores 4 and 5, identical. None of the defined bits (6, 7, 8, 11, 14, 26, 27, 28) is set. More importantly `L2ACTLR_EL1` is an *override* register, not a status register: `FORCE_*_CLK_ACTIVE = 0` means software has not forced the clock, which is the reset state, and says nothing about whether the clock runs. Answering that needs a CRU/PMU clock **status** register, not a CPU override register |

### What remains

The control experiment changes the question. The A72 cluster can receive
interrupts and does so under Linux with the identical BL31, so the redistributor,
its CPU interface and the per-cluster AXI4-Stream link are all working. The
differences between the working and failing case are therefore all inside
H-Exo, and the measurements in this section were taken in a GIC state that
H-Exo itself configured.

The prime suspect is an ordering defect in H-Exo's own GIC bring-up:

* `gicv3_init_cpu_iface()` runs on each secondary inside `smp_secondary_main()`,
  which is invoked from `smp_init()`.
* `gicv3_init()` runs on core 0 **after** `smp_init()` and begins with
  `gicd_write(GICD_CTLR, 0)`, disabling the Distributor wholesale, before
  re-asserting `ARE_S | ARE_NS | EnableGrp0 | EnableGrp1NS`.

So the per-CPU CPU interfaces are configured while the Distributor is still in
BL31's state, and the Distributor is then disabled and reprogrammed underneath
them. Linux never does this; it applies a specific workaround for what it calls
the insecure RK3399 GIC integration. **This is a hypothesis, not a conclusion.**
What is unexplained is why the A53 secondaries survive the same sequence while
the A72 do not, and that difference has not yet been identified.

An `A72_PROBE_B` verdict of `FORWARD_PATH_DEAD` taken inside H-Exo cannot settle
this, because the probe observes a GIC that H-Exo has already reconfigured.

**No fix is known.** Do not treat the CI patch in
`.github/workflows/rk3399-full-boot-firmware.yml` as one: it was measured to be
a no-op and is kept only as a labelled experiment.

### Remaining leads

1. **`rk3399_bl31_v1.36.elf` from rkbin.** The only remaining unexplored
   artefact. It is a prebuilt Rockchip BL31 with no public source, so it cannot
   be reasoned about - only disassembled and diffed against the TF-A v2.14.0
   path. If it configures the A72 redistributor differently, that is the answer.
   Nothing else known to the project has been left unmeasured.
2. **CRU/PMU clock status for the A72 L2 GIC-timer domain.** Follows from the
   `L2ACTLR_EL1` result above: the override register is clean, so the question
   has to be asked of the clock controller. The register has not been located;
   it needs the RK3399 TRM clock-gate table.

### Hazards to respect before changing anything

* Both `ChildrenAsleep` poll loops in TF-A's `gicv3_rdistif_mark_core_awake()`
  (`drivers/arm/gic/v3/gicv3_helpers.c`) are **unbounded** - no counter, no
  `udelay`, no timeout, and no `WARN` on the second one. Contrast the loops in
  `pmu.c`, which all bound themselves. Consequence: if any change ever makes
  `ProcessorSleep` read as `1` on an A72, BL31 will spin in EL3 forever and the
  core will never reach EL2. This must be bounded before any WAKER write is
  attempted.
* TF-A returns immediately from `mark_core_awake()` when `ProcessorSleep` is
  already 0 (`gicv3_helpers.c:41-43`), which is why the A72 frames are left at
  `WAKER = 0x4` and why the experimental hook in the CI workflow reads
  `ChildrenAsleep` as 0 and returns.
* Never write CCI-500 or `PMU_CCI500_CON` from Non-Secure EL2 (ADL-005). NS
  writes create `CHANGE_PENDING` transactions that never complete.

### Diagnostic bugs found and fixed en route

`A72_PROBE_B` used to print `ICC_HPPIR1_EL1` read *before* the pending bit was
injected. That value is trivially `0x3FF` and could never distinguish a live
forward path from a dead one, so its earlier "FORWARD_PATH_DEAD" verdict proved
nothing. Fixed in commit `a25a3449`; the verdict now derives from `hppir_after`,
with `before`, `after` and `final` all printed.

The same class of bug appeared twice and is worth naming, because it produces a
*false* result rather than a crash. `A72_PROBE_B` read a register before
injecting the thing it was supposed to observe. A patch can also be wrongly
declared missing: the `L2ACTLR` SiP patch adds no string literals, only macros,
two `static` functions and a `case`, so grepping the built `bl31.elf` for
"L2ACTLR" finds nothing and looks like a failed build. It had to be verified with
`nm -a` plus `objdump`, where the inlined `mrs x0, s3_1_c15_c0_0`
(`d539f000`) is present. **Verify a patch reached the binary by symbols or
disassembly, never by grepping for strings it does not contain.**

---

### ADL-006 - A72 Interrupt Non-Delivery Is an H-Exo GIC Bring-Up Defect

> **Superseded the same day.** The original text of this entry read "Below the
> Software Interface" and concluded no firmware programming could affect the
> fault. Both halves of that were wrong, and the error was to reason from
> measurements taken inside the system under investigation.

- **Decision**: The A72 cores are brought up correctly by BL31 and the defect is
  in H-Exo's own GIC bring-up, not in the hardware and not in BL31. Treat this as
  a GIC initialisation-order bug in H-Exo, and stop hunting the SoC.
- **Reason**: Ten hypotheses eliminated by measurement (table above). Then the
  decisive one: Armbian Linux booted from the same card with the **same BL31
  binary** brings up 6/6 processors and cores 4 and 5 take thousands of
  interrupts (`arch_timer` 3131 and 2953 on CPUs 4 and 5, `IPI1` 4338 and 4610).
  Hardware, redistributor, CPU interface and the per-cluster AXI4-Stream link all
  work. Every measurement in this entry was, however, taken inside H-Exo, which
  configures the GIC - so none of them was admissible evidence about the hardware.
- **Prime suspect**: `gicv3_init()` runs after `smp_init()` and starts with
  `gicd_write(GICD_CTLR, 0)`, disabling the Distributor after the per-CPU CPU
  interfaces were already programmed. HYPOTHESIS, not yet confirmed.
- **Alternative rejected**: keep hunting CCI snooping, ADB400, `ChildrenAsleep`
  or `L2ACTLR_EL1` - all measured, all clean. Ten cycles were spent there.
- **Method rule that follows from this**: never use the system under test as the
  instrument. A probe running inside the suspect sees the suspect's own
  configuration, which is how ten plausible hypotheses survived a whole
  investigation and all were wrong. The control has to be a different program on
  the same hardware.
- **Date**: 2026-10-02, revised the same day

---