# Fix SD card boot issues
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  SD Card Boot Repair" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Find SD card drive
Write-Host "[*] Looking for SD card..." -ForegroundColor Yellow
$drives = Get-Volume | Where-Object { 
    $_.DriveType -eq 'Removable' -and $_.DriveLetter 
}

if ($drives.Count -eq 0) {
    Write-Host "[!] No removable drives found. Insert SD card and run again." -ForegroundColor Red
    exit 1
}

Write-Host "[+] Found removable drives:" -ForegroundColor Green
foreach ($drive in $drives) {
    Write-Host "    $($drive.DriveLetter): - $($drive.FileSystemLabel) - $([math]::Round($drive.Size/1GB, 2)) GB" -ForegroundColor White
}

if ($drives.Count -gt 1) {
    Write-Host ""
    Write-Host "[!] Multiple drives found. Please specify drive letter:" -ForegroundColor Yellow
    $driveLetter = Read-Host "Enter drive letter (e.g., D)"
} else {
    $driveLetter = $drives[0].DriveLetter
    Write-Host ""
    Write-Host "[*] Using drive $driveLetter`:" -ForegroundColor Yellow
}

$sdRoot = "${driveLetter}:\"
$bootDir = "${driveLetter}:\boot"

if (-not (Test-Path $sdRoot)) {
    Write-Host "[!] Drive $driveLetter`: not accessible." -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "[*] Checking SD card contents..." -ForegroundColor Yellow

# Check for problematic files
$bootScr = Join-Path $bootDir "boot.scr"
$bootScrBak = Join-Path $bootDir "boot.scr.bak"
$kernelOld = Join-Path $bootDir "kernel_neuro.bin.old"
$kernelCurrent = Join-Path $bootDir "kernel_neuro.bin"

Write-Host ""
Write-Host "Files found:" -ForegroundColor Cyan

if (Test-Path $bootScr) {
    $size = (Get-Item $bootScr).Length
    Write-Host "  [!] boot.scr - $size bytes (THIS MAY CAUSE BOOT HANG)" -ForegroundColor Red
} else {
    Write-Host "  [ ] boot.scr - not found" -ForegroundColor Gray
}

if (Test-Path $kernelCurrent) {
    $size = (Get-Item $kernelCurrent).Length
    Write-Host "  [+] kernel_neuro.bin - $size bytes" -ForegroundColor Green
} else {
    Write-Host "  [ ] kernel_neuro.bin - not found" -ForegroundColor Gray
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "RECOMMENDED ACTIONS:" -ForegroundColor Yellow
Write-Host "========================================" -ForegroundColor Cyan

if (Test-Path $bootScr) {
    Write-Host "1. RENAME boot.scr to boot.scr.bak (prevents auto-boot script)" -ForegroundColor White
    $action1 = Read-Host "   Do this? (y/n)"
    
    if ($action1 -eq 'y') {
        try {
            if (Test-Path $bootScrBak) {
                Remove-Item $bootScrBak -Force
            }
            Rename-Item $bootScr $bootScrBak -Force
            Write-Host "   [+] boot.scr renamed to boot.scr.bak" -ForegroundColor Green
        } catch {
            Write-Host "   [!] Error: $($_.Exception.Message)" -ForegroundColor Red
        }
    }
}

Write-Host ""
Write-Host "2. BACKUP current kernel_neuro.bin (if exists)" -ForegroundColor White
if (Test-Path $kernelCurrent) {
    $action2 = Read-Host "   Do this? (y/n)"
    
    if ($action2 -eq 'y') {
        try {
            if (Test-Path $kernelOld) {
                Remove-Item $kernelOld -Force
            }
            Copy-Item $kernelCurrent $kernelOld -Force
            Write-Host "   [+] Backed up to kernel_neuro.bin.old" -ForegroundColor Green
        } catch {
            Write-Host "   [!] Error: $($_.Exception.Message)" -ForegroundColor Red
        }
    }
}

Write-Host ""
Write-Host "3. DELETE kernel_neuro.bin (force clean boot)" -ForegroundColor White
if (Test-Path $kernelCurrent) {
    $action3 = Read-Host "   Do this? (y/n)"
    
    if ($action3 -eq 'y') {
        try {
            Remove-Item $kernelCurrent -Force
            Write-Host "   [+] kernel_neuro.bin deleted" -ForegroundColor Green
        } catch {
            Write-Host "   [!] Error: $($_.Exception.Message)" -ForegroundColor Red
        }
    }
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "NEXT STEPS:" -ForegroundColor Yellow
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "1. Safely eject SD card from PC" -ForegroundColor White
Write-Host "2. Insert SD card back into NanoPi M4" -ForegroundColor White
Write-Host "3. Power cycle the board (remove power, wait 5 sec, reconnect)" -ForegroundColor White
Write-Host "4. Run: .\continuous_listen.ps1" -ForegroundColor White
Write-Host "5. You should see U-Boot boot messages now" -ForegroundColor White
Write-Host ""
Write-Host "If U-Boot appears, run: .\deploy_tftp_and_boot.ps1 -AssumePrompt" -ForegroundColor Yellow
Write-Host ""
