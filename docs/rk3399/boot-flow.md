# RK3399 Boot Flow

## High-Level Chain

For AArch64 RK3399 systems, the normal boot model is:

`BootROM -> TPL/SPL (or Rockchip miniloader) -> BL31 -> BL33`

In practice for this project:

- `idbloader.img` carries the early loader stages used to init DRAM
- `u-boot.itb` carries U-Boot proper and, in mainline-style flows, BL31
- legacy / BSP style setups may also use `trust.img` for BL31 / BL3x payloads

## Important Practical Fact

On RK3399, a lot of confusion comes from mixing two boot layouts:

### Mainline-like layout

- `idbloader.img` at sector `64`
- `u-boot.itb` at sector `16384`

### Legacy Rockchip layout

- trust / BL31 content may live separately
- logs like `Trust Addr:0x4000` and `Load uboot, ReadLba = 2000` indicate that the active chain is using a legacy split arrangement

If logs show:

- `Trust Addr:0x4000`
- `Load uboot, ReadLba = 2000`

then flashing only a random `u-boot.itb` to the wrong slot does not necessarily update the real boot path.

## Offsets Worth Remembering

Common RK3399 sector positions:

- `64` / `0x40`: `idbloader`
- `16384` / `0x4000`: U-Boot slot in many documented layouts
- `24576` / `0x6000`: trust / BL31 area in many split layouts

Your actual active chain must be validated against UART logs, not assumed from generic docs.

## Bootloader Safety Rule

Never flash `idbloader` blindly on NanoPi M4.

Reason:

- NanoPi M4 exists in both `2GB DDR3` and `4GB LPDDR3` versions
- wrong DDR init in TPL / SPL can hard-break boot before UART becomes useful

## For This Project

Before changing or flashing boot components, check:

- current UART boot log
- memory type of the board
- whether the chain is legacy split or mainline packed
- whether BL31 is in `trust.img` or inside `u-boot.itb`

## Local Authority Files

- `third_party/trusted-firmware-a/docs/plat/rockchip.rst`
- `third_party/u-boot/configs/nanopi-m4-rk3399_defconfig`
- `third_party/u-boot/dts/upstream/src/arm64/rockchip/rk3399-nanopi-m4.dts`
