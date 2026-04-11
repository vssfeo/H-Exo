# NanoPi M4 Board And Memory Notes

## Board Facts

The project target is `FriendlyElec NanoPi M4` based on `RK3399`.

Official board facts relevant to bring-up:

- SoC: `RK3399`
- CPU: `2x Cortex-A72 + 4x Cortex-A53`
- debug UART: `UART2`, `1500000 bps`

## Critical Memory Fact

NanoPi M4 ships in at least two RAM variants:

- `4GB LPDDR3`
- `2GB DDR3`

This matters directly for bootloader flashing.

## Why This Is Dangerous

Early boot stages are responsible for DRAM init.

If TPL / SPL / DDR blob is built for the wrong memory type:

- DRAM init fails early
- board may fall back to BootROM
- UART output may stop after TPL / DDR training errors

Example failure pattern:

- `LPDDR3 - 933MHz failed`
- `rk3399_dmc_init DRAM init failed`
- `Returning to boot ROM`

## Rule For This Project

- updating `u-boot.itb` can be acceptable if the current boot chain and slot are understood
- updating `idbloader` is forbidden unless DDR type is explicitly verified first

## Relevant Board Pointers

- FriendlyElec wiki states the board has both `2GB DDR3` and `4GB LPDDR3` versions
- the debug UART is 3V TTL and runs at `1500000`

## Local Source / Doc Anchors

- `third_party/u-boot/dts/upstream/src/arm64/rockchip/rk3399-nanopi-m4.dts`
- `third_party/u-boot/dts/upstream/src/arm64/rockchip/rk3399-nanopi-m4.dtsi`
- `third_party/u-boot/configs/nanopi-m4-rk3399_defconfig`
