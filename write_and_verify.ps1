#Requires -RunAsAdministrator

$DiskNumber = 1
$KernelPath = "C:\Users\SERYOGA\AndroidStudioProjects\H-Exo\kernel.bin"
$KernelSector = 500000  # Decimal: 500000 sectors = ~256MB offset, beyond Armbian
$SectorSize = 512

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  Kernel Writer & Verifier" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

if (-not (Test-Path $KernelPath)) {
    Write-Host "[-] kernel.bin not found!" -ForegroundColor Red
    exit 1
}

$KernelBytes = [System.IO.File]::ReadAllBytes($KernelPath)
Write-Host "[*] Kernel size: $($KernelBytes.Length) bytes" -ForegroundColor Yellow
Write-Host "[*] First 16 bytes: $([BitConverter]::ToString($KernelBytes[0..15]))" -ForegroundColor Yellow

# Pad to sector size
$remainder = $KernelBytes.Length % $SectorSize
if ($remainder -ne 0) {
    $paddingSize = $SectorSize - $remainder
    $padding = New-Object byte[] $paddingSize
    $KernelBytes += $padding
    Write-Host "[*] Padded to $($KernelBytes.Length) bytes" -ForegroundColor Yellow
}

$sectors = [int]($KernelBytes.Length / $SectorSize)
Write-Host "[*] Target: PhysicalDrive$DiskNumber, sector $KernelSector ($sectors sectors)" -ForegroundColor Yellow
Write-Host ""

# Write
$DrivePath = "\\.\PhysicalDrive$DiskNumber"
try {
    $stream = [System.IO.FileStream]::new($DrivePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::ReadWrite)
    $byteOffset = [int64]($KernelSector * $SectorSize)
    
    Write-Host "[*] Seeking to offset $byteOffset (sector $KernelSector)..." -ForegroundColor Yellow
    $stream.Seek($byteOffset, [System.IO.SeekOrigin]::Begin) | Out-Null
    
    Write-Host "[*] Writing $($KernelBytes.Length) bytes..." -ForegroundColor Yellow
    $stream.Write($KernelBytes, 0, $KernelBytes.Length)
    $stream.Flush()
    
    Write-Host "[+] Write completed!" -ForegroundColor Green
    
    # Verify
    Write-Host "`n[*] Verifying write..." -ForegroundColor Yellow
    $stream.Seek($byteOffset, [System.IO.SeekOrigin]::Begin) | Out-Null
    $readBack = New-Object byte[] $KernelBytes.Length
    $stream.Read($readBack, 0, $readBack.Length) | Out-Null
    
    $stream.Close()
    
    # Compare first 64 bytes
    $match = $true
    for ($i = 0; $i -lt [Math]::Min(64, $KernelBytes.Length); $i++) {
        if ($KernelBytes[$i] -ne $readBack[$i]) {
            $match = $false
            Write-Host "[-] Mismatch at byte ${i}: wrote $($KernelBytes[$i].ToString('X2')), read $($readBack[$i].ToString('X2'))" -ForegroundColor Red
            break
        }
    }
    
    if ($match) {
        Write-Host "[+] Verification PASSED!" -ForegroundColor Green
        Write-Host "[*] Read back first 16 bytes: $([BitConverter]::ToString($readBack[0..15]))" -ForegroundColor Green
    } else {
        Write-Host "[-] Verification FAILED!" -ForegroundColor Red
        exit 1
    }
    
    Write-Host "`n========================================" -ForegroundColor Cyan
    Write-Host "[+] Kernel successfully written to SD card" -ForegroundColor Green
    Write-Host "`nU-Boot commands:" -ForegroundColor Yellow
    Write-Host "  mmc dev 1" -ForegroundColor Cyan
    Write-Host "  mmc read 0x02080000 $KernelSector $sectors" -ForegroundColor Cyan
    Write-Host "  go 0x02080000" -ForegroundColor Cyan
    Write-Host "========================================`n" -ForegroundColor Cyan
    
} catch {
    Write-Host "[-] ERROR: $_" -ForegroundColor Red
    exit 1
}
