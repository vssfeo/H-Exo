#Requires -Version 7
<#
.SYNOPSIS
  Read raw trust region from SD/USB (physical LBA 0x6000, 4 MiB) — verify what is on card vs trust.img.
.DESCRIPTION
  Requires Administrator. Compare SHA256 to C:\tftpboot\trust.img or run inspect_trust_img.ps1 on the dump.
.PARAMETER DiskNumber
  Get-Disk Number for the SD in reader (NOT system disk).
.PARAMETER StartLba
  Default 0x6000 — Armbian-style trust slot (see recover_sdcard.ps1).
.PARAMETER ByteCount
  Bytes to read (default 4194304 = 8192 sectors).
.PARAMETER OutPath
  If set, write dump to this file (e.g. .\readback-trust.img).
.PARAMETER AutoRemovable
  If exactly one USB/removable disk in 1–128 GiB range, use it without prompt.
.PARAMETER Quiet
  Minimal output (orchestrators); still writes -OutPath when set.
#>
param(
    [int]$DiskNumber = -1,
    [string]$StartLba = "0x6000",
    [long]$ByteCount = 4194304,
    [string]$OutPath = "",
    [switch]$AutoRemovable,
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run PowerShell as Administrator."
}

$sector = 512
$lba = [Convert]::ToInt64($StartLba, 16)
$byteOffset = $lba * $sector
if ($ByteCount % $sector -ne 0) { throw "ByteCount must be multiple of 512." }

if (-not $Quiet) {
    Write-Host "=== Read trust region from physical disk ===" -ForegroundColor Cyan
    Write-Host "LBA 0x$([Convert]::ToString($lba, 16)), byte offset 0x$([Convert]::ToString($byteOffset, 16)), length $ByteCount bytes" -ForegroundColor Cyan
    Write-Host "`n=== Removable / USB disks ===" -ForegroundColor Yellow
    Get-Disk | Where-Object { $_.BusType -eq 'USB' -or $_.IsRemovable } |
        Format-Table Number, FriendlyName, @{N='GiB';E={[math]::Round($_.Size/1GB,2)}}, PartitionStyle, OperationalStatus
}

if ($DiskNumber -lt 0) {
    if ($AutoRemovable) {
        $cand = Get-Disk | Where-Object {
            ($_.BusType -eq 'USB' -or $_.IsRemovable) -and $_.Size -gt 1GB -and $_.Size -lt 130GB
        }
        if ($cand.Count -eq 1) {
            $DiskNumber = $cand[0].Number
            Write-Host "[*] -AutoRemovable: using Disk $DiskNumber ($($cand[0].FriendlyName))" -ForegroundColor Green
        }
        else {
            throw "AutoRemovable: expected exactly one USB/removable 1–128 GiB disk; found $($cand.Count). Pass -DiskNumber explicitly."
        }
    }
    else {
        throw "Pass -DiskNumber <N> from the table above, or -AutoRemovable if only one SD is connected."
    }
}

$d = Get-Disk -Number $DiskNumber
if (-not $Quiet) {
    Write-Host "`n>>> Reading Disk $DiskNumber — $($d.FriendlyName) — $([math]::Round($d.Size/1GB, 2)) GiB <<<" -ForegroundColor Yellow
}

$buf = New-Object byte[] $ByteCount
$diskPath = "\\.\PhysicalDrive$DiskNumber"
$stream = [System.IO.File]::Open($diskPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
try {
    $null = $stream.Seek($byteOffset, [System.IO.SeekOrigin]::Begin)
    $read = $stream.Read($buf, 0, $ByteCount)
    if ($read -ne $ByteCount) { throw "Short read: got $read bytes, expected $ByteCount" }
}
finally {
    $stream.Dispose()
}

$hash = [System.BitConverter]::ToString(
    [System.Security.Cryptography.SHA256]::HashData($buf)
).Replace("-", "")
if ($Quiet) {
    Write-Host "readback SHA256: $hash" -ForegroundColor DarkGray
}
else {
    Write-Host "`nSHA256 (readback): $hash" -ForegroundColor Green
    $latin1 = [System.Text.Encoding]::GetEncoding(28591)
    $s = $latin1.GetString($buf)
    $m = [regex]::Match($s, 'v2\.1[0-9]{1,2}(\.[0-9]+)?[^\x00]{0,80}')
    if ($m.Success) {
        Write-Host "TF-A / version-like snippet: $($m.Value.Trim())" -ForegroundColor Cyan
    }
    if ($s.Contains('Rockchip release')) {
        Write-Host "[!] Contains 'Rockchip release' (vendor BL31 style)." -ForegroundColor Yellow
    }
}

if ($OutPath) {
    $outFull = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutPath)
    [System.IO.File]::WriteAllBytes($outFull, $buf)
    if (-not $Quiet) { Write-Host "Wrote dump: $outFull" -ForegroundColor Green }
}

if (-not $Quiet) {
    $tftp = 'C:\tftpboot\trust.img'
    if (Test-Path -LiteralPath $tftp) {
        $ref = (Get-FileHash -Algorithm SHA256 -LiteralPath $tftp).Hash
        Write-Host "SHA256 (C:\tftpboot\trust.img): $ref" -ForegroundColor DarkGray
        if ($hash -eq $ref) {
            Write-Host "[OK] Readback MATCHES tftpboot trust.img" -ForegroundColor Green
        }
        else {
            Write-Host "[!] Readback differs from tftpboot trust.img (different image or wrong LBA/disk)." -ForegroundColor Red
        }
    }
    Write-Host "`nDone." -ForegroundColor DarkGray
}
