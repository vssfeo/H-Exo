# H-Exo Raw Sector Deployment Script
# Writes kernel_neuro.bin directly to SD card raw sectors
# Target: Sector 32600 (after U-Boot, before first partition)

param(
    [string]$KernelPath = ".\kernel_neuro.bin",
    [int]$TargetSector = 32600,
    [string]$PhysicalDrive = "\\.\PhysicalDrive1"  # WARNING: Verify this is your SD card!
)

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  H-Exo Raw Sector Deployment" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Check if running as Administrator
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "[!] ERROR: This script requires Administrator privileges!" -ForegroundColor Red
    Write-Host "[*] Please run PowerShell as Administrator and try again." -ForegroundColor Yellow
    exit 1
}

# Check if kernel file exists
if (-not (Test-Path $KernelPath)) {
    Write-Host "[!] ERROR: Kernel file not found: $KernelPath" -ForegroundColor Red
    exit 1
}

$kernelSize = (Get-Item $KernelPath).Length
Write-Host "[*] Kernel file: $KernelPath" -ForegroundColor Green
Write-Host "[*] Kernel size: $kernelSize bytes" -ForegroundColor Green
Write-Host "[*] Target drive: $PhysicalDrive" -ForegroundColor Yellow
Write-Host "[*] Target sector: $TargetSector (offset: $($TargetSector * 512) bytes)" -ForegroundColor Yellow
Write-Host ""

# Safety confirmation
Write-Host "[!] WARNING: This will write directly to raw sectors on $PhysicalDrive" -ForegroundColor Red
Write-Host "[!] Make sure this is the correct SD card drive!" -ForegroundColor Red
Write-Host ""
$confirmation = Read-Host "Type 'YES' to continue"
if ($confirmation -ne "YES") {
    Write-Host "[*] Deployment cancelled." -ForegroundColor Yellow
    exit 0
}

Write-Host ""
Write-Host "[*] Opening physical drive..." -ForegroundColor Cyan

try {
    # Open physical drive for writing
    $drive = [System.IO.File]::Open(
        $PhysicalDrive,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::ReadWrite
    )
    
    # Calculate byte offset
    $byteOffset = [long]$TargetSector * 512
    
    Write-Host "[*] Seeking to sector $TargetSector (offset: $byteOffset bytes)..." -ForegroundColor Cyan
    $drive.Seek($byteOffset, [System.IO.SeekOrigin]::Begin) | Out-Null
    
    # Read kernel binary
    Write-Host "[*] Reading kernel binary..." -ForegroundColor Cyan
    $kernelData = [System.IO.File]::ReadAllBytes($KernelPath)
    
    # Write to raw sectors
    Write-Host "[*] Writing $kernelSize bytes to raw sectors..." -ForegroundColor Cyan
    $drive.Write($kernelData, 0, $kernelData.Length)
    $drive.Flush()
    
    Write-Host "[+] Kernel successfully written to sector $TargetSector!" -ForegroundColor Green
    
    # Calculate sectors used
    $sectorsUsed = [Math]::Ceiling($kernelSize / 512)
    Write-Host "[*] Sectors used: $sectorsUsed (sector $TargetSector to $($TargetSector + $sectorsUsed - 1))" -ForegroundColor Green
    
    $drive.Close()
    
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "  Deployment Complete!" -ForegroundColor Green
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "[*] To boot from SD card in U-Boot:" -ForegroundColor Yellow
    Write-Host "    => mmc dev 1" -ForegroundColor White
    Write-Host "    => mmc read 0x02080000 $TargetSector $sectorsUsed" -ForegroundColor White
    Write-Host "    => go 0x02080000" -ForegroundColor White
    Write-Host ""
    
} catch {
    Write-Host "[!] ERROR: Failed to write to physical drive!" -ForegroundColor Red
    Write-Host "[!] $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
