#Requires -Version 7
<#
.SYNOPSIS
  Raw-write trust.img to NanoPi M4 (Armbian-style) SD card at physical LBA 0x6000.
.DESCRIPTION
  Use when UART/TFTP is unavailable or you want to repair trust from a PC card reader.
  Requires Administrator. WRONG DISK = DATA LOSS — triple-check DiskNumber.
.PARAMETER TrustPath
  Path to trust.img (BL31-only recommended for Armbian SPL).
.PARAMETER StartLba
  Physical sector (512 B). Default 0x6000 matches SPL "Trust" slot for this layout (see recover_sdcard.ps1).
.PARAMETER DiskNumber
  Windows disk index from Get-Disk (the SD card in USB reader, NOT your system NVMe).
#>
param(
    [Parameter(Mandatory = $true)][string]$TrustPath,
    [string]$StartLba = "0x6000",
    [int]$DiskNumber = -1
)

$ErrorActionPreference = "Stop"
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run PowerShell as Administrator."
}

$fullTrust = (Resolve-Path -LiteralPath $TrustPath).Path
$bytes = [System.IO.File]::ReadAllBytes($fullTrust)
$sector = 512
$rem = $bytes.Length % $sector
if ($rem -ne 0) {
    $pad = New-Object byte[] ($sector - $rem)
    $bytes = $bytes + $pad
}

$lba = [Convert]::ToInt64($StartLba, 16)
$byteOffset = $lba * $sector
Write-Host "trust.img: $($bytes.Length) bytes ($('{0:X}' -f $bytes.Length)), start LBA 0x$([Convert]::ToString($lba, 16)) (byte offset 0x$([Convert]::ToString($byteOffset, 16)))" -ForegroundColor Cyan

Write-Host "`n=== Get-Disk (removable) ===" -ForegroundColor Yellow
Get-Disk | Where-Object { $_.BusType -eq 'USB' -or $_.IsRemovable } | Format-Table Number, FriendlyName, Size, PartitionStyle, OperationalStatus

if ($DiskNumber -lt 0) {
    Write-Host "Enter DiskNumber for the SD card (see Number column): " -ForegroundColor Yellow -NoNewline
    $DiskNumber = [int](Read-Host)
}

$d = Get-Disk -Number $DiskNumber
Write-Host "`n>>> TARGET: Disk $DiskNumber — $($d.FriendlyName) — $([math]::Round($d.Size/1GB, 2)) GiB <<<" -ForegroundColor Red
Write-Host "Type YES to write trust to LBA 0x$([Convert]::ToString($lba, 16)): " -ForegroundColor Red -NoNewline
if ((Read-Host) -ne "YES") { Write-Host "Aborted."; exit 0 }

$diskPath = "\\.\PhysicalDrive$DiskNumber"
$stream = [System.IO.File]::Open($diskPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
try {
    $null = $stream.Seek($byteOffset, [System.IO.SeekOrigin]::Begin)
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
}
finally {
    $stream.Dispose()
}

Write-Host "`n[OK] Wrote $($bytes.Length) bytes to PhysicalDrive$DiskNumber at LBA 0x$([Convert]::ToString($lba, 16))." -ForegroundColor Green
Write-Host "Safely eject SD, insert in M4, power on." -ForegroundColor Cyan
