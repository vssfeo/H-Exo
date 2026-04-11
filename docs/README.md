# H-Exo Docs Hub

This `docs/` folder is the local knowledge base for RK3399 / NanoPi M4 bring-up.

Use it before searching the web.

## Start Here

- `docs/INDEX.md`: topic index and search tags
- `docs/rk3399/boot-flow.md`: boot chain, flash layout, offsets
- `docs/rk3399/board-and-memory.md`: board revision and DDR3 vs LPDDR3 facts
- `docs/rk3399/smp-bringup.md`: PSCI, TF-A warmboot path, common bring-up traps
- `docs/rk3399/source-map.md`: where the truth lives in `third_party/`
- `tools/search-rk3399-docs.ps1`: fast search across curated docs and key source files

## Working Rule

For this project, prefer this order:

1. Search `docs/`
2. Search the mapped source files in `third_party/`
3. Only then search the web

## Quick Search

In PowerShell:

```powershell
.\tools\search-rk3399-docs.ps1 psci
.\tools\search-rk3399-docs.ps1 warmboot
.\tools\search-rk3399-docs.ps1 "DDR3 LPDDR3"
```

## What This Docs Pack Covers

- RK3399 boot ROM and boot stages
- NanoPi M4 board facts relevant to bring-up
- DDR3 vs LPDDR3 risk for bootloader flashing
- TF-A / BL31 role on RK3399
- Rockchip PMU warmboot path for secondary CPUs
- U-Boot image layout and flash offsets
- Local source files that must be checked before changing SMP logic
