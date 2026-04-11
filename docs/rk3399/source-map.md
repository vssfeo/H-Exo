# RK3399 Source Map

This is the local map of files that matter most for bring-up.

## Board Definition

- `third_party/u-boot/dts/upstream/src/arm64/rockchip/rk3399-nanopi-m4.dts`
- `third_party/u-boot/dts/upstream/src/arm64/rockchip/rk3399-nanopi-m4.dtsi`
- `third_party/u-boot/configs/nanopi-m4-rk3399_defconfig`

Use these for:

- board identity
- UART defaults
- U-Boot board config
- board-specific device-tree facts

## TF-A Platform Docs

- `third_party/trusted-firmware-a/docs/plat/rockchip.rst`

Use this for:

- official Rockchip TF-A boot chain
- supported platform model
- build expectations for BL31

## TF-A PSCI And Warmboot Path

- `third_party/trusted-firmware-a/plat/rockchip/common/plat_pm.c`
- `third_party/trusted-firmware-a/plat/rockchip/common/aarch64/plat_helpers.S`
- `third_party/trusted-firmware-a/plat/rockchip/common/aarch64/pmu_sram_cpus_on.S`
- `third_party/trusted-firmware-a/plat/rockchip/common/bl31_plat_setup.c`
- `third_party/trusted-firmware-a/plat/rockchip/common/include/plat_private.h`
- `third_party/trusted-firmware-a/plat/rockchip/common/plat_topology.c`

Use these for:

- PSCI CPU_ON handling
- secure warmboot path
- per-core entrypoint handling
- MPIDR to linear core mapping

## RK3399 PMU Power Control

- `third_party/trusted-firmware-a/plat/rockchip/rk3399/drivers/pmu/pmu.c`
- `third_party/trusted-firmware-a/plat/rockchip/rk3399/include/platform_def.h`
- `third_party/trusted-firmware-a/plat/rockchip/rk3399/rk3399_def.h`

Use these for:

- core and cluster power sequencing
- PMU init
- secondary power-on details
- warmboot address setup

## Secure Payload Interaction

- `third_party/trusted-firmware-a/services/spd/opteed/opteed_pm.c`

Use this for:

- OP-TEE cpu-on finish path
- secure world secondary handoff

## Project Kernel Side

- `boot.s`
- `core/smp.c`
- `core/smp.h`
- `main_neuro.c`
- `hal/gicv3.c`
- `hal/cci.c`
- `linker.ld`

Use these only after the TF-A and PMU path is understood.
