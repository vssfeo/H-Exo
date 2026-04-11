# RK3399 Firmware Handoff Debug — ROOT CAUSE FOUND

## THE BUG (найдено 2026-04-08)

**Наш trust.img (4MB) содержит BL31 + OP-TEE v2.12 как BL32.**

Armbian работает потому что их trust.img (2MB) содержит ТОЛЬКО BL31, без OP-TEE.

Путь secondary CPU с нашим trust:
1. PSCI CPU_ON → BL31 warmboot
2. BL31 вызывает `opteed_cpu_on_finish_handler()` — OP-TEE должен инициализироваться на вторичном ядре
3. OP-TEE v2.12 не завершает `cpu_on_entry` корректно (несовместимость версий или конфигурации)
4. Secondary CPU навсегда застревает в secure world
5. Ядро never reached → `path=pre-kernel-handoff` навсегда

**Решение**: flash `trust-armbian-no-optee.img` (Armbian BL31 без OP-TEE).
Нашему нейро-ядру OP-TEE не нужен вообще.

Команда:
```powershell
.\flash_bootloader_uboot.ps1 -TrustOnly -TrustFile "trust-armbian-no-optee.img" -MmcDev 1 -ForceWrite
```

---


This note tracks the BL31 / OP-TEE side of secondary CPU bring-up.

## Why This Exists

Current kernel telemetry shows:

- PSCI `CPU_ON` accepted
- some cores remain `ON_PENDING`
- one core may become `AFF_STATE_ON`
- no secondary reaches kernel `_start`

That means the likely fault is before non-secure kernel execution.

## Instrumented Files

- `third_party/trusted-firmware-a/bl31/bl31_main.c`
- `third_party/trusted-firmware-a/lib/psci/psci_common.c`
- `third_party/trusted-firmware-a/services/spd/opteed/opteed_pm.c`

## New UART Trace Prefix

The firmware trace prints lines prefixed with:

- `H-EXO:`

## Expected Trace Interpretation

### If secondary reaches BL31 warmboot

You should see:

- `H-EXO: bl31 warmboot cpu=X`

### If secondary reaches PSCI warmboot completion

You should see:

- `H-EXO: psci warmboot cpu=X ...`
- `H-EXO: psci cpu_on_finish cpu=X`

### If secondary reaches OP-TEE cpu_on_finish

You should see:

- `H-EXO: opteed cpu_on_finish start cpu=X`
- `H-EXO: opteed cpu_on_finish enter sp cpu=X`
- `H-EXO: opteed cpu_on_finish return cpu=X rc=...`
- `H-EXO: opteed cpu_on_finish done cpu=X`

### If secondary is ready to leave EL3

You should see:

- `H-EXO: warmboot ep cpu=X pc=... spsr=...`
- `H-EXO: prepare el3 exit ns cpu=X`
- `H-EXO: set domains RUN cpu=X aff=...`

## Bypass Test

`opteed_pm.c` now has a debug switch:

- `HEXO_OPTEED_BYPASS_SECONDARY_CPU_ON`

When set to `1`, non-primary CPUs skip OP-TEE `cpu_on_finish` and continue as if it succeeded.

This is a pure A/B test:

- if secondaries suddenly reach kernel code, OP-TEE secondary handoff is the blocker
- if nothing changes, the fault is earlier in BL31 / PSCI / PMU
