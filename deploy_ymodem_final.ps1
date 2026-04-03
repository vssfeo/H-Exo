# Final deployment via YMODEM (loady)
param(
    [int]$BaudRate = 1500000
)

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  H-Exo YMODEM Deploy (Final Method)" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

if (-not (Test-Path ".\kernel_neuro.bin")) {
    Write-Host "[!] kernel_neuro.bin not found!" -ForegroundColor Red
    exit 1
}

$kernelSize = (Get-Item ".\kernel_neuro.bin").Length
Write-Host "[*] Kernel size: $kernelSize bytes" -ForegroundColor Yellow
Write-Host ""
Write-Host "INSTRUCTIONS:" -ForegroundColor Cyan
Write-Host "1. This script will connect to U-Boot" -ForegroundColor White
Write-Host "2. Send 'loady 0x02080000' command" -ForegroundColor White
Write-Host "3. Transfer kernel via YMODEM" -ForegroundColor White
Write-Host "4. Execute 'go 0x02080000' to start kernel" -ForegroundColor White
Write-Host ""
Write-Host "Power cycle the board now and press Enter..." -ForegroundColor Yellow
Read-Host

# Kill any process holding COM3
Get-Process | Where-Object {
    ($_.ProcessName -eq "pwsh" -or $_.ProcessName -eq "powershell") -and $_.Id -ne $PID
} | Stop-Process -Force -ErrorAction SilentlyContinue

Start-Sleep -Seconds 1

try {
    $port = New-Object System.IO.Ports.SerialPort
    $port.PortName = "COM3"
    $port.BaudRate = $BaudRate
    $port.DataBits = 8
    $port.Parity = [System.IO.Ports.Parity]::None
    $port.StopBits = [System.IO.Ports.StopBits]::One
    $port.Handshake = [System.IO.Ports.Handshake]::None
    $port.ReadTimeout = 1000
    $port.WriteTimeout = 1000
    $port.DtrEnable = $true
    $port.RtsEnable = $true
    
    $port.Open()
    Write-Host "[+] COM3 opened at $BaudRate baud" -ForegroundColor Green
    
    $port.DiscardInBuffer()
    $port.DiscardOutBuffer()
    
    # Wait for U-Boot prompt
    Write-Host "[*] Waiting for U-Boot prompt (sending Enter periodically)..." -ForegroundColor Yellow
    Write-Host ""
    
    $buf = ""
    $deadline = [DateTime]::Now.AddSeconds(90)
    $promptFound = $false
    $lastEnter = [DateTime]::Now
    
    while ([DateTime]::Now -lt $deadline) {
        if (([DateTime]::Now - $lastEnter).TotalMilliseconds -ge 300) {
            $port.Write([char]13)
            $port.Write([char]3)
            $lastEnter = [DateTime]::Now
        }
        
        if ($port.BytesToRead -gt 0) {
            $chunk = $port.ReadExisting()
            $buf += $chunk
            Write-Host $chunk -NoNewline -ForegroundColor Gray
            
            if ($buf -match "=>" -or $buf -match "U-Boot>") {
                $promptFound = $true
                break
            }
        }
        Start-Sleep -Milliseconds 50
    }
    
    if (-not $promptFound) {
        Write-Host "`n[!] U-Boot prompt not detected within 90 seconds." -ForegroundColor Red
        Write-Host "    Manually interrupt autoboot and run:" -ForegroundColor Yellow
        Write-Host "    loady 0x02080000" -ForegroundColor White
        Write-Host "    Then use lrzsz or TeraTerm to send kernel_neuro.bin via YMODEM" -ForegroundColor White
        Write-Host "    Finally: go 0x02080000" -ForegroundColor White
        exit 1
    }
    
    Write-Host "`n`n[+] U-Boot prompt detected!" -ForegroundColor Green
    Write-Host ""
    
    # Send loady command
    Write-Host "[>] loady 0x02080000" -ForegroundColor Yellow
    $port.WriteLine("loady 0x02080000")
    Start-Sleep -Seconds 2
    
    # Read response
    $loadyBuf = ""
    $deadline = [DateTime]::Now.AddSeconds(5)
    while ([DateTime]::Now -lt $deadline) {
        if ($port.BytesToRead -gt 0) {
            $chunk = $port.ReadExisting()
            $loadyBuf += $chunk
            Write-Host $chunk -NoNewline -ForegroundColor White
        }
        Start-Sleep -Milliseconds 100
    }
    
    if ($loadyBuf -match "Ready for binary" -or $loadyBuf -match "CCC") {
        Write-Host "`n[+] U-Boot ready for YMODEM transfer!" -ForegroundColor Green
        Write-Host ""
        Write-Host "========================================" -ForegroundColor Red
        Write-Host "MANUAL STEP REQUIRED:" -ForegroundColor Red
        Write-Host "========================================" -ForegroundColor Red
        Write-Host "1. Keep this window open" -ForegroundColor White
        Write-Host "2. Open PuTTY or TeraTerm connected to COM3 at $BaudRate baud" -ForegroundColor White
        Write-Host "3. Send kernel_neuro.bin via YMODEM (File -> Transfer -> YMODEM -> Send)" -ForegroundColor White
        Write-Host "4. Wait for transfer to complete (~20KB at $BaudRate baud = ~1-2 seconds)" -ForegroundColor White
        Write-Host "5. After transfer, press Enter here to continue..." -ForegroundColor White
        Write-Host ""
        
        Read-Host "Press Enter after YMODEM transfer completes"
        
        # Send go command
        Write-Host ""
        Write-Host "[>] go 0x02080000" -ForegroundColor Yellow
        $port.WriteLine("go 0x02080000")
        Start-Sleep -Seconds 1
        
        # Monitor kernel output
        Write-Host ""
        Write-Host "=== KERNEL OUTPUT ===" -ForegroundColor Cyan
        Write-Host "Looking for boot beacons: A, B, C and BEAT messages..." -ForegroundColor Yellow
        Write-Host ""
        
        $monitorDeadline = [DateTime]::Now.AddSeconds(60)
        while ([DateTime]::Now -lt $monitorDeadline) {
            if ($port.BytesToRead -gt 0) {
                Write-Host $port.ReadExisting() -NoNewline -ForegroundColor White
            }
            Start-Sleep -Milliseconds 20
        }
        
        Write-Host ""
        Write-Host ""
        Write-Host "[+] Monitoring complete." -ForegroundColor Green
        
    } else {
        Write-Host "`n[!] U-Boot did not enter YMODEM receive mode." -ForegroundColor Red
        Write-Host "    Output: $loadyBuf" -ForegroundColor Gray
    }
    
} catch {
    Write-Host "`n[ERROR] $($_.Exception.Message)" -ForegroundColor Red
} finally {
    if ($port -and $port.IsOpen) {
        $port.Close()
        $port.Dispose()
        Write-Host "`n[!] Port closed." -ForegroundColor Yellow
    }
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Alternative: Use lrzsz command-line tool" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "If you have lrzsz installed:" -ForegroundColor White
Write-Host "  sz -vv --ymodem kernel_neuro.bin > COM3" -ForegroundColor Gray
Write-Host ""
