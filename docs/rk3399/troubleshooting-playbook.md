# RK3399 Troubleshooting Playbook

Use this after `smp_dump_diagnostics()` or when the board misbehaves.

## 1. Board No Longer Boots

### Symptoms

- TPL prints DDR training errors
- boot falls back to BootROM
- no usable U-Boot prompt

### Likely Cause

- wrong early boot image for the board memory type
- `idbloader` or DDR init blob built for `LPDDR3` on a `DDR3` board, or vice versa

### What To Check

- `docs/rk3399/board-and-memory.md`
- current UART boot log
- whether `idbloader` was changed

### Rule

Do not reflash `idbloader` unless memory type is verified.

## 2. PSCI CPU_ON Returns Success But Kernel Sees No Secondary

### Symptoms

- `ret=0`
- affinity may change
- no `_start` tombstone
- no `secondary_entry` beacon
- no C worker entry

### Meaning

This is not proof of kernel failure.
It usually means the chain broke before non-secure kernel execution:

`CPU_ON -> PMU warmboot -> secure on_finish -> normal world handoff`

### What To Check

- `docs/rk3399/smp-bringup.md`
- `third_party/trusted-firmware-a/plat/rockchip/common/plat_pm.c`
- `third_party/trusted-firmware-a/plat/rockchip/common/aarch64/plat_helpers.S`
- `third_party/trusted-firmware-a/plat/rockchip/rk3399/drivers/pmu/pmu.c`
- `third_party/trusted-firmware-a/services/spd/opteed/opteed_pm.c`

### Path Classification

The current runtime now interprets bring-up like this:

- `not-requested`: `CPU_ON` was not accepted
- `pre-kernel-handoff`: PSCI accepted but kernel telemetry saw nothing
- `_start`: secondary reached kernel start trampoline
- `secondary-entry`: secondary reached early ASM entry
- `c-worker`: secondary reached `smp_secondary_main()`

## 3. How Runtime Should Behave While SMP Is Broken

### Rule

The project must remain functional on core 0.

### Expected Behavior

- telemetry still updates
- adaptive inference still runs
- networking still works
- workqueue offload is used only if a secondary reached `c-worker`

### Why

This prevents project progress from stalling on unresolved SMP bring-up.

## 4. Fast Search

Use:

```powershell
.\tools\search-rk3399-docs.ps1 warmboot
.\tools\search-rk3399-docs.ps1 cpuson_entry_point
.\tools\search-rk3399-docs.ps1 "DDR3 LPDDR3"
.\tools\search-rk3399-docs.ps1 "pre-kernel-handoff"
```
