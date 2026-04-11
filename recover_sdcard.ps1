
# recover_sdcard.ps1
# Restores Armbian LOADER2 to physical LBA 0x5000 on SD card.
# Run AFTER removing SD card from NanoPi M4 and inserting into PC card reader.
#
# Physical layout (NanoPi M4 / Armbian DDR3):
#   FwPartOffset = 0x2000 sectors (SPL addresses are relative to this)
#   Physical 0x4000 = SPL ReadLba 0x2000 : LOADER1 / u-boot.itb FIT
#   Physical 0x5000 = SPL ReadLba 0x3000 : LOADER2  <-- U-Boot loads from here!
#   Physical 0x6000 = SPL Trust 0x4000   : trust.img (BL31)
#
# What went wrong: mmc write to U-Boot block 0x4000 wrote to physical 0x4000-0x5FFF,
# which overwrote LOADER2 at physical 0x5000.

[CmdletBinding()]
param(
    [string]$LoaderFile  = "C:\tftpboot\u-boot-working-from-armbian-loader2.bin",
    [int]$DiskNumber     = -1,    # -1 = auto-detect / prompt
    [string]$PhysicalLba = ""     # override physical LBA (e.g. "0x6000" for trust.img)
)

$ErrorActionPreference = "Stop"

# Default = LOADER2 at 0x5000; override with -PhysicalLba for trust.img at 0x6000
if ($PhysicalLba) {
    $WRITE_LBA = [Convert]::ToInt64($PhysicalLba, 16)
} else {
    $WRITE_LBA = 0x5000
}
$LOADER2_PHYSICAL_LBA = $WRITE_LBA
$LOADER2_BYTE_OFFSET  = $WRITE_LBA * 512

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  NanoPi M4 SD Card Recovery Script" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Restores Armbian LOADER2 to physical LBA 0x5000 (byte offset 0x$([Convert]::ToString($LOADER2_BYTE_OFFSET,16)))"
Write-Host ""

# Verify loader file
if (-not (Test-Path $LoaderFile)) {
    Write-Host "[ERROR] Loader file not found: $LoaderFile" -ForegroundColor Red
    exit 1
}
$loaderBytes = [System.IO.File]::ReadAllBytes($LoaderFile)
$magic = [System.Text.Encoding]::ASCII.GetString($loaderBytes[0..7])
Write-Host "Loader file: $LoaderFile"
Write-Host "  Size:  $($loaderBytes.Length) bytes ($('{0:X}' -f $loaderBytes.Length) hex)"
Write-Host "  Magic: '$magic' (should be 'LOADER  ')"
if ($loaderBytes[0] -ne 0x4C -or $loaderBytes[1] -ne 0x4F) {
    Write-Host "[WARN] Unexpected magic - not Rockchip LOADER format" -ForegroundColor Yellow
}
Write-Host ""

# List physical disks
Write-Host "=== Physical Disks ===" -ForegroundColor Cyan
$disks = Get-Disk | Sort-Object Number
foreach ($disk in $disks) {
    $size_gb = [math]::Round($disk.Size / 1GB, 1)
    $flag = if ($disk.IsRemovable) { "[REMOVABLE]" } else { "" }
    Write-Host ("  Disk {0}: {1} ({2} GB) - {3} {4}" -f $disk.Number, $disk.FriendlyName, $size_gb, $disk.PartitionStyle, $flag) -ForegroundColor $(if ($disk.IsRemovable) { "Yellow" } else { "Gray" })
}
Write-Host ""

# Auto-select or prompt
if ($DiskNumber -ge 0) {
    $selectedDisk = $DiskNumber
} else {
    $removable = $disks | Where-Object { $_.IsRemovable -and $_.Size -gt 1GB -and $_.Size -lt 128GB }
    if ($removable.Count -eq 1) {
        $selectedDisk = $removable[0].Number
        $sz = [math]::Round($removable[0].Size / 1GB, 1)
        Write-Host "[*] Auto-selected Disk $selectedDisk ($($removable[0].FriendlyName), $sz GB)" -ForegroundColor Yellow
    } else {
        Write-Host "Enter disk number for SD card (e.g. 1): " -ForegroundColor Yellow -NoNewline
        $selectedDisk = [int](Read-Host)
    }
}

# Final confirmation
$d = Get-Disk -Number $selectedDisk
$size_gb = [math]::Round($d.Size / 1GB, 1)
Write-Host ""
Write-Host ">>> SELECTED: Disk $selectedDisk - $($d.FriendlyName) - $size_gb GB <<<" -ForegroundColor Red
Write-Host ">>> WRITE TARGET: byte offset 0x$([Convert]::ToString($LOADER2_BYTE_OFFSET,16)) (physical LBA 0x$([Convert]::ToString($LOADER2_PHYSICAL_LBA,16)))" -ForegroundColor Red
Write-Host ">>> FILE: $LoaderFile ($($loaderBytes.Length) bytes)" -ForegroundColor Red
Write-Host ""
Write-Host "This will restore LOADER2 (U-Boot) to the SD card." -ForegroundColor Green
Write-Host "Type YES to continue: " -ForegroundColor Yellow -NoNewline
$confirm = Read-Host
if ($confirm -ne "YES") {
    Write-Host "Aborted."
    exit 0
}

# Write to SD card
Write-Host ""
Write-Host "[*] Opening disk $selectedDisk for raw write..."
$diskPath = "\\.\PhysicalDrive$selectedDisk"

try {
    $stream = [System.IO.File]::Open($diskPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    
    Write-Host "[*] Seeking to byte offset 0x$([Convert]::ToString($LOADER2_BYTE_OFFSET,16))..."
    $stream.Seek($LOADER2_BYTE_OFFSET, [System.IO.SeekOrigin]::Begin) | Out-Null
    
    Write-Host "[*] Writing $($loaderBytes.Length) bytes..."
    $stream.Write($loaderBytes, 0, $loaderBytes.Length)
    $stream.Flush()
    $stream.Close()
    
    Write-Host ""
    Write-Host "[OK] LOADER2 written successfully to physical LBA 0x5000!" -ForegroundColor Green
    Write-Host ""
    Write-Host "Next steps:" -ForegroundColor Cyan
    Write-Host "  1. Remove SD card from PC"
    Write-Host "  2. Insert into NanoPi M4"
    Write-Host "  3. Power on - board should boot U-Boot"
    Write-Host "  4. Then run flash_bootloader_uboot.ps1 -TrustOnly -TrustLba 0x6000"
    Write-Host "     (Trust is at physical LBA 0x6000 = U-Boot block 0x6000)"
    Write-Host ""
    Write-Host "IMPORTANT FIX: All future mmc write commands use PHYSICAL LBAs (no FwPartOffset)." -ForegroundColor Yellow
    Write-Host "  SPL addresses are relative to FwPartOffset=0x2000, but U-Boot uses absolute." -ForegroundColor Yellow
    Write-Host "  SPL ReadLba 0x2000 = U-Boot block 0x4000 (physical LBA 0x4000)" -ForegroundColor Yellow
    Write-Host "  SPL ReadLba 0x3000 = U-Boot block 0x5000 (physical LBA 0x5000)" -ForegroundColor Yellow
    Write-Host "  SPL Trust 0x4000   = U-Boot block 0x6000 (physical LBA 0x6000)" -ForegroundColor Yellow
}
catch {
    Write-Host ""
    Write-Host "[ERROR] Failed to write: $_" -ForegroundColor Red
    Write-Host "Make sure:" -ForegroundColor Yellow
    Write-Host "  - Running as Administrator"
    Write-Host "  - SD card is inserted"
    Write-Host "  - No other process is using the disk"
}
