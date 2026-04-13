#Requires -Version 7
<#
.SYNOPSIS
  Raw-write trust.img to NanoPi M4 (Armbian-style) SD card at physical LBA 0x6000.
.DESCRIPTION
  Use when UART/TFTP is unavailable or you want to repair trust from a PC card reader.
  Requires Administrator. WRONG DISK = DATA LOSS — triple-check DiskNumber.
  Before overwriting, saves the current region to boot-backups/trust-dsk{N}-lba….img (unless -NoBackup).
.PARAMETER TrustPath
  Path to trust.img (BL31-only recommended for Armbian SPL).
.PARAMETER StartLba
  Physical sector (512 B). Default 0x6000 matches SPL "Trust" slot for this layout (see recover_sdcard.ps1).
.PARAMETER DiskNumber
  Windows disk index from Get-Disk (the SD card in USB reader, NOT your system NVMe).
.PARAMETER ForceWrite
  Skip the interactive YES prompt (automation only). Refused for NVMe; allowed only for USB or removable disks.
.PARAMETER NoBackup
  Do not read and save the previous contents of the trust region before writing (not recommended).
.PARAMETER BackupDir
  Folder for backup .img files (default: <repo>/boot-backups).
.PARAMETER MaxBackups
  After saving a new backup, delete older backup files for this same DiskNumber in BackupDir, keeping this many newest (0 = keep all).
#>
param(
    [Parameter(Mandatory = $true)][string]$TrustPath,
    [string]$StartLba = "0x6000",
    [int]$DiskNumber = -1,
    [switch]$ForceWrite,
    [switch]$NoBackup,
    [string]$BackupDir = "",
    [int]$MaxBackups = 15
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
Write-Host "    BusType=$($d.BusType)  IsRemovable=$($d.IsRemovable)" -ForegroundColor DarkGray

if ($ForceWrite) {
    if ($d.BusType -eq 'NVMe') {
        throw "-ForceWrite refused: will not auto-write to NVMe (Disk $DiskNumber)."
    }
    if ($d.BusType -ne 'USB' -and -not $d.IsRemovable) {
        throw "-ForceWrite refused: disk $DiskNumber is not USB and not IsRemovable — use interactive YES or pick the SD disk."
    }
    Write-Host "[*] -ForceWrite: USB/removable target OK, skipping YES prompt." -ForegroundColor Yellow
}
else {
    Write-Host "Type YES to write trust to LBA 0x$([Convert]::ToString($lba, 16)): " -ForegroundColor Red -NoNewline
    if ((Read-Host) -ne "YES") { Write-Host "Aborted."; exit 0 }
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if ($BackupDir) {
    $backupRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BackupDir)
}
else {
    $backupRoot = Join-Path $repoRoot 'boot-backups'
}
if (-not $NoBackup) {
    if (-not (Test-Path -LiteralPath $backupRoot)) {
        New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
    }
    $ts = Get-Date -Format 'yyyyMMdd-HHmmss'
    $backupPath = Join-Path $backupRoot ("trust-dsk{0}-lba{1:X}-{2}.img" -f $DiskNumber, $lba, $ts)
    Write-Host "`n[*] Backup: reading $($bytes.Length) B from LBA 0x$([Convert]::ToString($lba, 16)) -> $(Split-Path $backupPath -Leaf)" -ForegroundColor Cyan
}

$diskPath = "\\.\PhysicalDrive$DiskNumber"
$stream = [System.IO.File]::Open($diskPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
try {
    $null = $stream.Seek($byteOffset, [System.IO.SeekOrigin]::Begin)
    if (-not $NoBackup) {
        $prev = New-Object byte[] $bytes.Length
        $r = $stream.Read($prev, 0, $prev.Length)
        if ($r -ne $prev.Length) {
            Write-Host "[WARN] Backup short read: $r of $($prev.Length) bytes (EOF or error)." -ForegroundColor Yellow
        }
        [System.IO.File]::WriteAllBytes($backupPath, $prev)
        $bh = (Get-FileHash -Algorithm SHA256 -LiteralPath $backupPath).Hash
        Write-Host "    SHA256: $bh" -ForegroundColor DarkGray
        if ($MaxBackups -gt 0) {
            $pat = "trust-dsk${DiskNumber}-lba*.img"
            $older = Get-ChildItem -LiteralPath $backupRoot -Filter $pat -File -ErrorAction SilentlyContinue |
                Sort-Object LastWriteTime -Descending |
                Select-Object -Skip $MaxBackups
            foreach ($o in $older) {
                Remove-Item -LiteralPath $o.FullName -Force
                Write-Host "    (trim) removed old backup: $($o.Name)" -ForegroundColor DarkGray
            }
        }
    }
    $null = $stream.Seek($byteOffset, [System.IO.SeekOrigin]::Begin)
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
}
finally {
    $stream.Dispose()
}

Write-Host "`n[OK] Wrote $($bytes.Length) bytes to PhysicalDrive$DiskNumber at LBA 0x$([Convert]::ToString($lba, 16))." -ForegroundColor Green
Write-Host "Safely eject SD, insert in M4, power on." -ForegroundColor Cyan
