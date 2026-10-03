<#
.SYNOPSIS
    Offline golden-log check: does a captured UART log still contain every
    structural marker H-Exo is supposed to print?

.DESCRIPTION
    The only previous check of boot behaviour lived in verify_phases.ps1 and needed
    a COM port attached to the board. That makes "did boot still work" a question
    only a human with hardware can answer, which is why a regression can sit in a
    log unnoticed until it turns up as a strange throughput number.

    This runs entirely on committed files. No board, no flashing, no serial port.

    Exit code 0 = every REQUIRED marker present. 1 = at least one missing.
    Informational markers (prefixed '@') are reported and never fail the run.

.EXAMPLE
    pwsh -File tools/check_markers.ps1 -Log log/1.txt
    pwsh -File tools/check_markers.ps1 -Dir log
    pwsh -File tools/check_markers.ps1 -Log run.log -Verbose
#>
[CmdletBinding()]
param(
    [string]$Log,
    [string]$Dir,
    [string]$Markers
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

$Repo = Split-Path -Parent $PSScriptRoot
if (-not $Markers) { $Markers = Join-Path $Repo 'docs\expected\markers.txt' }
if (-not (Test-Path $Markers)) {
    Write-Host "FAIL  marker contract not found: $Markers" -ForegroundColor Red
    exit 2
}

$required = New-Object System.Collections.Generic.List[string]
$optional = New-Object System.Collections.Generic.List[string]
foreach ($line in Get-Content $Markers) {
    $t = $line.Trim()
    if ($t -eq '' -or $t.StartsWith('#')) { continue }
    if ($t.StartsWith('@')) { $optional.Add($t.Substring(1).Trim()) } else { $required.Add($t) }
}
if ($required.Count -eq 0) {
    Write-Host "FAIL  the marker contract has no REQUIRED markers; it would pass anything" -ForegroundColor Red
    exit 2
}

$logs = @()
if ($Log)  { $logs += $Log }
if ($Dir)  { $logs += @(Get-ChildItem -Path $Dir -Filter '*.txt' -File | ForEach-Object { $_.FullName }) }
if ($logs.Count -eq 0) {
    Write-Host "FAIL  give -Log <file> or -Dir <folder>" -ForegroundColor Red
    exit 2
}

$failed = $false

# Logs captured before a feature existed cannot satisfy the current contract, and
# that is not a regression. The exclusion list keeps "-Dir log" green without
# weakening the contract for anything captured from now on. It applies ONLY to
# directory scans: naming a log explicitly with -Log is a deliberate choice by the
# caller and is always checked.
$historical = @{}
$histFile = Join-Path $Repo 'docs\expected\historical-logs.txt'
if (Test-Path $histFile) {
    foreach ($line in Get-Content $histFile) {
        $t = $line.Trim()
        if ($t -eq '' -or $t.StartsWith('#')) { continue }
        $historical[(Split-Path -Leaf $t)] = $true
    }
}

foreach ($p in $logs) {
    if (-not (Test-Path $p)) {
        Write-Host "FAIL  log not found: $p" -ForegroundColor Red
        $failed = $true
        continue
    }
    $leaf = Split-Path -Leaf $p
    if ($Dir -and -not $Log -and $historical.ContainsKey($leaf)) {
        Write-Host ""
        Write-Host "== $leaf  SKIPPED (historical, listed in docs/expected/historical-logs.txt)" -ForegroundColor DarkGray
        continue
    }
    Write-Host ""
    Write-Host "== $leaf  ($((Get-Item $p).Length) bytes)" -ForegroundColor Cyan
    $text = Get-Content $p -Raw

    $miss = @()
    foreach ($m in $required) {
        if ($text -notmatch [regex]::Escape($m)) { $miss += $m }
    }
    $present = $required.Count - $miss.Count
    if ($miss.Count -eq 0) {
        Write-Host "   OK    all $($required.Count) required markers present" -ForegroundColor Green
    } else {
        Write-Host "   FAIL  $($miss.Count) of $($required.Count) required markers missing" -ForegroundColor Red
        foreach ($m in $miss) { Write-Host "           missing: $m" -ForegroundColor Red }
        $failed = $true
    }

    $missInfo = @()
    foreach ($m in $optional) {
        if ($text -notmatch [regex]::Escape($m)) { $missInfo += $m }
    }
    Write-Host "   ..    informational: $($optional.Count - $missInfo.Count)/$($optional.Count) present" -ForegroundColor DarkGray
    if ($missInfo.Count -gt 0) {
        Write-Host "         absent (not a failure): $($missInfo -join ', ')" -ForegroundColor DarkGray
    }
}

Write-Host ""
if ($failed) {
    Write-Host "FAIL - at least one log is missing a required marker" -ForegroundColor Red
    exit 1
}
Write-Host "PASS - every checked log satisfies the marker contract" -ForegroundColor Green
exit 0
