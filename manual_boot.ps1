param(
    [string]$PortName = "COM3"
)

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  H-Exo Manual Boot Monitor" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

Write-Host "[*] Instructions:" -ForegroundColor Yellow
Write-Host "    1. This script will open serial monitor" -ForegroundColor Gray
Write-Host "    2. PHYSICALLY REBOOT the NanoPi M4 (power cycle)" -ForegroundColor Gray
Write-Host "    3. Press Ctrl+C when you see U-Boot prompt" -ForegroundColor Gray
Write-Host "    4. Manually type commands in U-Boot:" -ForegroundColor Gray
Write-Host "       => mmc dev 1" -ForegroundColor DarkGray
Write-Host "       => mmc read 0x02080000 500000 28" -ForegroundColor DarkGray
Write-Host "       => go 0x02080000" -ForegroundColor DarkGray
Write-Host "    5. Select option 2 (Heartbeat Mode)" -ForegroundColor Gray
Write-Host ""

# Try different baud rates
$baudRates = @(1500000, 115200)

foreach ($baud in $baudRates) {
    Write-Host "[*] Trying to open $PortName at $baud baud..." -ForegroundColor Yellow
    
    try {
        $port = New-Object System.IO.Ports.SerialPort $PortName, $baud, None, 8, One
        $port.Handshake = [System.IO.Ports.Handshake]::None
        $port.ReadTimeout = 500
        $port.WriteTimeout = 500
        $port.Open()
        
        Write-Host "[+] Serial port opened at $baud baud" -ForegroundColor Green
        Write-Host "[*] Monitoring output... (Ctrl+C to stop)" -ForegroundColor Yellow
        Write-Host "[*] REBOOT THE BOARD NOW!" -ForegroundColor Red
        Write-Host ""
        
        $lastActivity = [DateTime]::Now
        $noActivityWarning = $false
        
        while ($true) {
            try {
                if ($port.BytesToRead -gt 0) {
                    $data = $port.ReadExisting()
                    Write-Host $data -NoNewline
                    $lastActivity = [DateTime]::Now
                    $noActivityWarning = $false
                    
                    # Detect important events
                    if ($data -match "U-Boot") {
                        Write-Host "`n[!] U-Boot detected!" -ForegroundColor Green
                    }
                    if ($data -match "=>") {
                        Write-Host "`n[!] U-Boot prompt detected - you can type commands now!" -ForegroundColor Green
                    }
                    if ($data -match "BEAT") {
                        Write-Host "`n[!] Heartbeat detected!" -ForegroundColor Green
                    }
                    if ($data -match "H-Exo") {
                        Write-Host "`n[!] H-Exo kernel booted!" -ForegroundColor Green
                    }
                } else {
                    # Check for inactivity
                    $inactiveSeconds = ([DateTime]::Now - $lastActivity).TotalSeconds
                    if ($inactiveSeconds -gt 10 -and -not $noActivityWarning) {
                        Write-Host "`n[WARN] No data for $([Math]::Round($inactiveSeconds))s - is the board powered on?" -ForegroundColor Yellow
                        $noActivityWarning = $true
                    }
                }
                Start-Sleep -Milliseconds 50
            } catch {
                # Ignore read timeouts
            }
        }
        
    } catch [System.UnauthorizedAccessException] {
        Write-Host "[ERROR] Port $PortName is already in use by another application" -ForegroundColor Red
        Write-Host "[*] Close any terminal programs (PuTTY, TeraTerm, etc.) and try again" -ForegroundColor Yellow
        exit 1
    } catch {
        Write-Host "[ERROR] Failed to open port at $baud baud: $($_.Exception.Message)" -ForegroundColor Red
        continue
    } finally {
        if ($null -ne $port -and $port.IsOpen) {
            $port.Close()
        }
    }
}

Write-Host "[ERROR] Could not open serial port at any baud rate" -ForegroundColor Red
