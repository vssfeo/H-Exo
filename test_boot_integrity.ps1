param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 1500000,
    [string]$KernelPath = "C:\Users\SERYOGA\AndroidStudioProjects\H-Exo\kernel_optimized.bin"
)

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  H-Exo Boot Integrity Test" -ForegroundColor Cyan
Write-Host "  Emergency Beacon Validation" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

$KernelSize = (Get-Item $KernelPath).Length
$SectorCount = [Math]::Ceiling($KernelSize / 512)

Write-Host "[*] Kernel: $KernelPath" -ForegroundColor Yellow
Write-Host "[*] Size: $KernelSize bytes ($SectorCount sectors)" -ForegroundColor Yellow
Write-Host ""
Write-Host "[*] Expected Beacon Sequence:" -ForegroundColor Yellow
Write-Host "    1 - Hardware under control" -ForegroundColor Gray
Write-Host "    2 - About to transition EL3->EL2" -ForegroundColor Gray
Write-Host "    3 - Successfully at EL2" -ForegroundColor Gray
Write-Host "    4 - Now at EL1, about to init MMU" -ForegroundColor Gray
Write-Host "    5 - MMU enabled, page tables working" -ForegroundColor Gray
Write-Host ""
Write-Host "[*] REBOOT THE BOARD NOW!" -ForegroundColor Yellow
Write-Host "========================================`n" -ForegroundColor Cyan

# Open serial port
$port = New-Object System.IO.Ports.SerialPort $PortName, $BaudRate, None, 8, One
$port.Handshake = [System.IO.Ports.Handshake]::None
$port.ReadTimeout = 30000
$port.WriteTimeout = 5000

try {
    $port.Open()
    Write-Host "[*] Serial port $PortName opened at $BaudRate baud" -ForegroundColor Green
    
    # Wait for U-Boot and aggressively interrupt autoboot
    Write-Host "[*] Waiting for U-Boot prompt..." -ForegroundColor Yellow
    $buffer = ""
    $timeout = [DateTime]::Now.AddSeconds(60)
    $promptDetected = $false
    
    while ([DateTime]::Now -lt $timeout) {
        if ($port.BytesToRead -gt 0) {
            $data = $port.ReadExisting()
            $buffer += $data
            Write-Host $data -NoNewline
            
            # Aggressively send Ctrl+C to interrupt Armbian autoboot
            if ($buffer -match "Hit any key to stop autoboot" -or $buffer -match "autoboot") {
                for ($i = 0; $i -lt 10; $i++) {
                    $port.Write([char]3)  # Ctrl+C
                    Start-Sleep -Milliseconds 50
                }
            }
            
            if ($buffer -match "=>") {
                Write-Host "`n[+] U-Boot prompt detected!" -ForegroundColor Green
                $promptDetected = $true
                break
            }
        }
        Start-Sleep -Milliseconds 100
    }
    
    if (-not $promptDetected) {
        Write-Host "[ERROR] U-Boot prompt not detected - Armbian may have booted" -ForegroundColor Red
        return
    }
    
    # CRITICAL: Close port before calling send_ymodem.ps1
    Write-Host "[*] Closing port for YMODEM transfer..." -ForegroundColor Yellow
    $port.Close()
    Start-Sleep -Seconds 1
    
    # Send YMODEM transfer command
    Write-Host "[*] Starting YMODEM transfer..." -ForegroundColor Yellow
    & "$PSScriptRoot\send_ymodem.ps1" `
        -PortName $PortName `
        -BaudRate $BaudRate `
        -FilePath $KernelPath `
        -LoadAddress 0x02080000 `
        -AutoBoot `
        -KernelSector 500000 `
        -KernelSectorCount $SectorCount
    
    # Reopen port for beacon validation
    Write-Host "[*] Reopening port for beacon capture..." -ForegroundColor Yellow
    Start-Sleep -Seconds 2
    $port.Open()
    
    Write-Host "`n========================================" -ForegroundColor Cyan
    Write-Host "  Beacon Sequence Validation" -ForegroundColor Cyan
    Write-Host "========================================`n" -ForegroundColor Cyan
    
    # Collect boot output
    $bootOutput = ""
    $beacons = @()
    $timeout = [DateTime]::Now.AddSeconds(10)
    
    while ([DateTime]::Now -lt $timeout) {
        if ($port.BytesToRead -gt 0) {
            $data = $port.ReadExisting()
            $bootOutput += $data
            Write-Host $data -NoNewline
            
            # Extract beacons
            foreach ($char in $data.ToCharArray()) {
                if ($char -match '[1-5]') {
                    $beacons += $char
                }
            }
        }
        Start-Sleep -Milliseconds 50
    }
    
    Write-Host "`n`n========================================" -ForegroundColor Cyan
    Write-Host "  Test Results" -ForegroundColor Cyan
    Write-Host "========================================`n" -ForegroundColor Cyan
    
    # Validate beacon sequence
    $expectedBeacons = @('1', '2', '3', '4', '5')
    $testPassed = $true
    
    Write-Host "Detected Beacons: $($beacons -join ', ')" -ForegroundColor Yellow
    Write-Host ""
    
    for ($i = 0; $i -lt $expectedBeacons.Length; $i++) {
        $expected = $expectedBeacons[$i]
        if ($i -lt $beacons.Length -and $beacons[$i] -eq $expected) {
            Write-Host "[PASS] Beacon $expected detected" -ForegroundColor Green
        } else {
            Write-Host "[FAIL] Beacon $expected missing or out of order" -ForegroundColor Red
            $testPassed = $false
            
            # Diagnose failure point
            switch ($expected) {
                '1' { Write-Host "       -> Hardware initialization failed" -ForegroundColor Red }
                '2' { Write-Host "       -> Failed before EL3->EL2 transition" -ForegroundColor Red }
                '3' { Write-Host "       -> EL3->EL2 transition crashed (check SPSR/ELR)" -ForegroundColor Red }
                '4' { Write-Host "       -> EL2->EL1 transition crashed" -ForegroundColor Red }
                '5' { Write-Host "       -> MMU initialization or page table error" -ForegroundColor Red }
            }
            break
        }
    }
    
    Write-Host ""
    
    # Check for expected kernel messages
    $expectedMessages = @(
        "[OK] Hardware: RK3399",
        "[OK] MMU: Enabled",
        "H-Exo Omni-Core: Operational"
    )
    
    Write-Host "Kernel Message Validation:" -ForegroundColor Yellow
    foreach ($msg in $expectedMessages) {
        if ($bootOutput -match [regex]::Escape($msg)) {
            Write-Host "[PASS] $msg" -ForegroundColor Green
        } else {
            Write-Host "[FAIL] Missing: $msg" -ForegroundColor Red
            $testPassed = $false
        }
    }
    
    Write-Host "`n========================================" -ForegroundColor Cyan
    if ($testPassed) {
        Write-Host "  BOOT INTEGRITY: PASSED" -ForegroundColor Green
    } else {
        Write-Host "  BOOT INTEGRITY: FAILED" -ForegroundColor Red
    }
    Write-Host "========================================`n" -ForegroundColor Cyan
    
} catch {
    Write-Host "[ERROR] $($_.Exception.Message)" -ForegroundColor Red
} finally {
    if ($port.IsOpen) {
        $port.Close()
    }
}
