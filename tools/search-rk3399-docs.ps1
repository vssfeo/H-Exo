$ErrorActionPreference = "Stop"

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Query
)

$repoRoot = Split-Path -Parent $PSScriptRoot

$targets = @(
    (Join-Path $repoRoot "docs"),
    (Join-Path $repoRoot "third_party\trusted-firmware-a\docs\plat\rockchip.rst"),
    (Join-Path $repoRoot "third_party\trusted-firmware-a\plat\rockchip\common\plat_pm.c"),
    (Join-Path $repoRoot "third_party\trusted-firmware-a\plat\rockchip\common\aarch64\plat_helpers.S"),
    (Join-Path $repoRoot "third_party\trusted-firmware-a\plat\rockchip\common\aarch64\pmu_sram_cpus_on.S"),
    (Join-Path $repoRoot "third_party\trusted-firmware-a\plat\rockchip\rk3399\drivers\pmu\pmu.c"),
    (Join-Path $repoRoot "third_party\trusted-firmware-a\services\spd\opteed\opteed_pm.c"),
    (Join-Path $repoRoot "third_party\u-boot\configs\nanopi-m4-rk3399_defconfig"),
    (Join-Path $repoRoot "third_party\u-boot\dts\upstream\src\arm64\rockchip\rk3399-nanopi-m4.dts"),
    (Join-Path $repoRoot "third_party\u-boot\dts\upstream\src\arm64\rockchip\rk3399-nanopi-m4.dtsi")
)

$existingTargets = $targets | Where-Object { Test-Path $_ }

if (-not $existingTargets) {
    throw "No search targets found."
}

Write-Host "Searching RK3399 docs for: $Query" -ForegroundColor Cyan
Write-Host ""

rg --line-number --ignore-case --context 2 --fixed-strings -- $Query $existingTargets
