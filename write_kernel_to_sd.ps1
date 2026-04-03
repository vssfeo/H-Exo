# Raw-write kernel binary to SD card at a fixed LBA (same as U-Boot mmc read/write in this project).
# Default sector 500000 (decimal) matches send_ymodem.ps1 -KernelSector and boot_from_sd.ps1.
# REQUIRES ADMIN PRIVILEGES

param(
    [string]$KernelPath = "kernel_neuro.bin",
    [int]$TargetSector = 500000,
    [int]$DiskNumber = -1
)

# Check admin
if (-NOT ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole] "Administrator")) {
    Write-Host "[ERROR] This script requires Administrator privileges!" -ForegroundColor Red
    Write-Host "[*] Right-click PowerShell and select 'Run as Administrator'" -ForegroundColor Yellow
    exit 1
}

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  H-Exo Kernel Writer to SD Card" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

# Load kernel
if (-not (Test-Path $KernelPath)) {
    Write-Host "[ERROR] Kernel not found: $KernelPath" -ForegroundColor Red
    exit 1
}

$kernel = [System.IO.File]::ReadAllBytes($KernelPath)
$kernelSize = $kernel.Length
$sectorCount = [Math]::Ceiling($kernelSize / 512)

Write-Host "[*] Kernel: $KernelPath" -ForegroundColor Yellow
Write-Host "[*] Size: $kernelSize bytes ($sectorCount sectors)" -ForegroundColor Yellow
Write-Host "[*] Target sector: $TargetSector (0x$($TargetSector.ToString('X')))" -ForegroundColor Yellow

# Find SD card - MUST be USB and reasonable size (< 128GB)
Write-Host "[*] Searching for SD card (USB device, < 128GB)..." -ForegroundColor Yellow

$allDisks = Get-Disk | Format-Table Number, FriendlyName, BusType, @{Label="Size(GB)";Expression={[Math]::Round($_.Size/1GB,2)}} -AutoSize | Out-String
Write-Host $allDisks

if ($DiskNumber -ge 0) {
    $disk = Get-Disk -Number $DiskNumber -ErrorAction SilentlyContinue
    if ($null -eq $disk) {
        Write-Host "[ERROR] No disk with number $DiskNumber" -ForegroundColor Red
        exit 1
    }
    if ($disk.BusType -ne "USB") {
        Write-Host "[ERROR] Disk $DiskNumber is not USB (BusType=$($disk.BusType)). Refusing for safety." -ForegroundColor Red
        exit 1
    }
    if ($disk.Number -eq 0) {
        Write-Host "[FATAL] Refusing to use Disk 0 (system disk)." -ForegroundColor Red
        exit 1
    }
} else {
    $disk = Get-Disk | Where-Object {
        $_.BusType -eq "USB" -and
        $_.Size -lt 128GB -and
        $_.Size -gt 1GB
    }

    if ($null -eq $disk) {
        Write-Host "[ERROR] SD card not found!" -ForegroundColor Red
        Write-Host "[*] Looking for: USB device with size between 1GB and 128GB" -ForegroundColor Yellow
        Write-Host "[*] Insert the card in a USB reader, or re-run with -DiskNumber N (from table above)" -ForegroundColor Yellow
        exit 1
    }

    if ($disk -is [array]) {
        Write-Host "[ERROR] Multiple USB storage devices found!" -ForegroundColor Red
        $disk | Format-Table Number, FriendlyName, BusType, Size -AutoSize
        Write-Host "[*] Re-run with -DiskNumber <N> for the correct SD (must be USB, not Disk 0)." -ForegroundColor Yellow
        exit 1
    }
}

# SAFETY CHECK: Verify it's NOT Disk 0 (system disk)
if ($disk.Number -eq 0) {
    Write-Host "[FATAL] Disk 0 detected - this is likely your system disk!" -ForegroundColor Red
    Write-Host "[FATAL] Refusing to write to Disk 0 for safety" -ForegroundColor Red
    exit 1
}

Write-Host "[*] Found SD card: $($disk.FriendlyName) (Disk $($disk.Number))" -ForegroundColor Green
Write-Host "[*] BusType: $($disk.BusType)" -ForegroundColor Green
Write-Host "[*] Size: $([Math]::Round($disk.Size / 1GB, 2)) GB" -ForegroundColor Green

# Double confirmation
Write-Host ""
Write-Host "========================================" -ForegroundColor Red
Write-Host "  FINAL SAFETY CHECK" -ForegroundColor Red
Write-Host "========================================" -ForegroundColor Red
Write-Host "Target disk: Disk $($disk.Number)" -ForegroundColor Yellow
Write-Host "Friendly name: $($disk.FriendlyName)" -ForegroundColor Yellow
Write-Host "Size: $([Math]::Round($disk.Size / 1GB, 2)) GB" -ForegroundColor Yellow
Write-Host "Sector: $TargetSector (0x$($TargetSector.ToString('X')))" -ForegroundColor Yellow
Write-Host ""
Write-Host "[WARNING] This will OVERWRITE data on this disk!" -ForegroundColor Red
Write-Host "[WARNING] Double-check this is your SD card, NOT your system disk!" -ForegroundColor Red
Write-Host ""

# Ask user to type disk number manually
$confirmNumber = Read-Host "Type the disk NUMBER to confirm ($($disk.Number))"

if ($confirmNumber -ne $disk.Number.ToString()) {
    Write-Host "[*] Disk number mismatch - aborted for safety" -ForegroundColor Yellow
    exit 0
}

$confirmYes = Read-Host "Type 'WRITE' in UPPERCASE to proceed"

if ($confirmYes -ne "WRITE") {
    Write-Host "[*] Aborted by user" -ForegroundColor Yellow
    exit 0
}

# Pad kernel to sector boundary
$paddedSize = $sectorCount * 512
$paddedKernel = New-Object byte[] $paddedSize
[Array]::Copy($kernel, $paddedKernel, $kernelSize)

Write-Host ""
Write-Host "[*] Writing kernel to sector $TargetSector..." -ForegroundColor Yellow

try {
    # Open disk for writing
    $diskPath = "\\.\PhysicalDrive$($disk.Number)"
    $stream = [System.IO.File]::Open($diskPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
    
    # Seek to target sector
    $offset = [long]$TargetSector * 512
    $stream.Seek($offset, [System.IO.SeekOrigin]::Begin) | Out-Null
    
    # Write kernel
    $stream.Write($paddedKernel, 0, $paddedSize)
    $stream.Flush()
    $stream.Close()
    
    Write-Host "[+] Kernel written successfully!" -ForegroundColor Green
    Write-Host "[*] Wrote $paddedSize bytes ($sectorCount sectors)" -ForegroundColor Green
    
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "  Next Steps" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "1. Insert SD card into NanoPi M4" -ForegroundColor Yellow
    Write-Host "2. Boot to U-Boot prompt" -ForegroundColor Yellow
    Write-Host "3. Run commands:" -ForegroundColor Yellow
    Write-Host "   => mmc dev 1" -ForegroundColor Gray
    Write-Host ("   => mmc dev 1; mmc read 0x02080000 {0} {1}; go 0x02080000" -f $TargetSector, $sectorCount) -ForegroundColor Gray
    Write-Host "   => go 0x02080000" -ForegroundColor Gray
    Write-Host "4. Select option 2 (Heartbeat Mode)" -ForegroundColor Yellow
    
} catch {
    Write-Host "[ERROR] Failed to write kernel: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
