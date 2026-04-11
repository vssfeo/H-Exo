# RK3399 / NanoPi M4 Index

Use this as the first stop when debugging bring-up.

## Topics

### Boot flow

- file: `docs/rk3399/boot-flow.md`
- tags: `bootrom`, `tpl`, `spl`, `bl31`, `bl33`, `u-boot`, `trust.img`, `idbloader`, `u-boot.itb`, `offsets`, `flash`

### Board and memory

- file: `docs/rk3399/board-and-memory.md`
- tags: `nanopi m4`, `friendlyelec`, `2gb`, `4gb`, `ddr3`, `lpddr3`, `uart`, `rev`, `schematic`

### SMP bring-up

- file: `docs/rk3399/smp-bringup.md`
- tags: `smp`, `psci`, `cpu_on`, `warmboot`, `pmu`, `pmu sram`, `optee`, `cci`, `gicv3`, `secondary_entry`, `mpidr`

### Roadmap & Achievement Log ← **START HERE for full history**

- file: `docs/rk3399/ROADMAP.md`
- tags: `roadmap`, `milestone`, `smp`, `solved`, `a53`, `a72`, `mmu`, `trampoline`, `adl`, `changelog`
- **STATUS 2026-04-08**: A53 cores 1–3 ONLINE ✅ | A72 cores 4–5 pending 🔄

### Troubleshooting

- file: `docs/rk3399/troubleshooting-playbook.md`
- tags: `troubleshooting`, `pre-kernel-handoff`, `bringup`, `boot fail`, `ddr3`, `lpddr3`, `c-worker`

### Source map

- file: `docs/rk3399/source-map.md`
- tags: `third_party`, `tf-a`, `u-boot`, `pmu.c`, `plat_helpers.S`, `plat_pm.c`, `nanopi-m4-rk3399_defconfig`

## Fast Heuristics

- If the issue is "board no longer boots": check `board-and-memory.md` and `boot-flow.md`
- If the issue is "secondary CPU never reaches kernel code": check `smp-bringup.md`
- If the issue is "which source file is authoritative": check `source-map.md`
- If the issue is "what do I flash and where": check `boot-flow.md`

## Search Keywords That Usually Matter

- `CPU_ON`
- `warmboot`
- `cpuson_entry_point`
- `cpuson_flags`
- `platform_cpu_warmboot`
- `PMU_CPU_HOTPLUG`
- `DDR3`
- `LPDDR3`
- `ReadLba = 2000`
- `Trust Addr:0x4000`
- `GICR_WAKER`
