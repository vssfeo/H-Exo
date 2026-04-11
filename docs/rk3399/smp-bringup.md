# RK3399 SMP Bring-Up Notes

## Core Insight

Do not model RK3399 secondary bring-up as a guaranteed direct branch from PSCI into the kernel non-secure entrypoint.

For Rockchip TF-A on RK3399, secondary CPUs go through a secure warmboot path first.

## What TF-A Actually Does

In Rockchip TF-A:

- `plat_setup_psci_ops()` stores the secondary entrypoint
- `rockchip_pwr_domain_on()` calls `rockchip_soc_cores_pwr_dm_on()`
- RK3399 PMU code writes:
  - `cpuson_flags[cpu_id]`
  - `cpuson_entry_point[cpu_id]`
- the secondary CPU wakes through `platform_cpu_warmboot()`
- only then does it branch to the per-CPU entrypoint

So the real chain is:

`PSCI CPU_ON -> EL3 / PMU power-on -> PMU SRAM warmboot code -> platform_cpu_warmboot -> cpuson_entry_point[cpu] -> next stage`

## Why This Matters

If a secondary core never reaches kernel `_start` or `secondary_entry`, the bug may be:

- before the non-secure handoff
- inside BL31 warmboot logic
- in cluster / PMU power domain sequencing
- in secure payload handoff such as OP-TEE cpu-on finish path

Not every failure is a kernel landing-point bug.

## Important TF-A Files

- `third_party/trusted-firmware-a/plat/rockchip/common/plat_pm.c`
- `third_party/trusted-firmware-a/plat/rockchip/common/aarch64/plat_helpers.S`
- `third_party/trusted-firmware-a/plat/rockchip/common/aarch64/pmu_sram_cpus_on.S`
- `third_party/trusted-firmware-a/plat/rockchip/rk3399/drivers/pmu/pmu.c`
- `third_party/trusted-firmware-a/services/spd/opteed/opteed_pm.c`

## Mapping Rule

For RK3399, the useful linear core map is:

- A53 cluster: `0..3`
- A72 cluster: `4..5`

Equivalent formula:

- `core_pos = Aff0 + (Aff1 << 2)`

This matches the Rockchip TF-A topology helper behavior.

## ✅ SOLVED (2026-04-08) — A53 Cores 1–3 Online

The final root cause and fix are documented in `ROADMAP.md` (ADL-002).

**Short version**: secondary cores after BL31 ERET have MMU/caches off. Instruction
fetch from 0x02081000 goes non-coherent AXI → SLVERR → Cortex-A53 POISON → EC=0 fault.
Fix: trampoline reads core 0's `TTBR0/TCR/MAIR` from `beacon[4..6]` and enables EL2 MMU
(`M+C+I=1`) before `br` to `secondary_entry`. Now fetch is Normal-Cacheable coherent.

See `ROADMAP.md` for full history, all dead ends, and the complete diagnostic trail.

## Common Bring-Up Traps

- assuming PSCI `ret=0` means the CPU already reached kernel code
- assuming secondary CPU enters the kernel directly from `CPU_ON`
- using full-system barriers too aggressively during cluster power-up
- reading stale cache lines on core 0 while secondaries write uncached or differently shared data
- ignoring OP-TEE / BL32 interaction on secondary `cpu_on_finish`

## Working Heuristic

If logs show:

- PSCI `CPU_ON` success
- affinity transitions or partial power changes
- but no `_start` / `secondary_entry` telemetry

then focus on:

- TF-A warmboot path
- PMU power-domain sequencing
- secure world handoff

before changing more kernel assembly.
