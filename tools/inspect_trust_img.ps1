#!/usr/bin/env pwsh
# Heuristic inspection of Rockchip trust.img (BL3X container). Not a full parser.
# Usage: .\tools\inspect_trust_img.ps1 [-TrustPath] .\trust.img

param(
    [Parameter(Mandatory = $false)]
    [string]$TrustPath = ".\trust.img"
)

$ErrorActionPreference = "Stop"
$full = (Resolve-Path -LiteralPath $TrustPath).Path
$bytes = [System.IO.File]::ReadAllBytes($full)
$len = $bytes.Length
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $full).Hash

# ISO-8859-1: O(1) byte->char map for whole-file string scan (avoid per-byte pipeline).
$latin1 = [System.Text.Encoding]::GetEncoding(28591)
$s = $latin1.GetString($bytes)

Write-Host "=== trust.img inspection ===" -ForegroundColor Cyan
Write-Host "Path:   $full"
Write-Host "Size:   $len bytes"
Write-Host "SHA256: $hash"

$hints = [System.Collections.Generic.List[string]]::new()

if ($s -match 'Trusted Firmware') {
    $hints.Add('Substring "Trusted Firmware" found — typical of upstream TF-A / ARM-TF BL31 payload.')
}
if ($s -match 'ARM Trusted Firmware') {
    $hints.Add('Substring "ARM Trusted Firmware" found — TF-A family.')
}
if ($s -match 'H-EXO:') {
    $hints.Add('Substring "H-EXO:" found — matches instrumented tree in third_party/trusted-firmware-a.')
}
if ($s -match 'Rockchip release') {
    $hints.Add('Substring "Rockchip release" found — Rockchip vendor BL31 (rkbin), not upstream TF-A banner.')
}
if ($s -match 'plat_rockchip') {
    $hints.Add('Substring "plat_rockchip" found — Rockchip platform layer in firmware image.')
}
if ($s -match 'opteed_fast|opteed_cpu_on|OP-TEE|optee') {
    $hints.Add('OP-TEE / opteed strings present — image may include BL32; see docs/rk3399/firmware-handoff-debug.md.')
}

$m = [regex]::Match($s, 'v2\.1[0-9]{1,2}(\.[0-9]+)?')
if ($m.Success) {
    $hints.Add("Possible TF-A style version token: $($m.Value) (confirm against your build tag).")
}

if ($len -eq 4194304) {
    $hints.Add('Size exactly 4 MiB — matches CI bl31_only trust.img profile in this repo.')
}
elseif ($len -eq 2097152) {
    $hints.Add('Size exactly 2 MiB — common Armbian-style trust.img size.')
}

if ($hints.Count -eq 0) {
    $hints.Add('No strong fingerprints — container may strip strings or use unfamiliar build; rely on SHA256 vs known artifact.')
}

Write-Host "`nHeuristics (Latin-1 scan of entire file):" -ForegroundColor Yellow
foreach ($h in $hints) {
    Write-Host "  • $h"
}

Write-Host "`nUART check: after flash, early boot should print BL31 build identity; compare to strings above." -ForegroundColor DarkGray
Write-Host "If UART still shows Rockchip Jul 2020 / v1.3 but this file shows TF-A strings, suspect wrong tftpboot file or SPL not loading this slot." -ForegroundColor DarkGray
