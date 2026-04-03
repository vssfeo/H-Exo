param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 1500000,
    [int]$DurationSeconds = 60
)

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  H-Exo Heartbeat Stability Test" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

$KernelPath = "C:\Users\SERYOGA\AndroidStudioProjects\H-Exo\kernel_neuro.bin"

# Force reload kernel file from disk (avoid caching)
if (Test-Path $KernelPath) {
    [System.GC]::Collect()
    [System.GC]::WaitForPendingFinalizers()
}

$KernelSize = (Get-Item $KernelPath).Length
$SectorCount = [Math]::Ceiling($KernelSize / 512)

Write-Host "[*] Kernel: $KernelPath" -ForegroundColor Yellow
Write-Host "[*] Size: $KernelSize bytes ($SectorCount sectors)" -ForegroundColor Yellow
Write-Host "[*] Modified: $((Get-Item $KernelPath).LastWriteTime)" -ForegroundColor Yellow
Write-Host "[*] Test duration: $DurationSeconds seconds" -ForegroundColor Yellow
Write-Host ""
Write-Host "[*] REBOOT THE BOARD NOW!" -ForegroundColor Yellow
Write-Host "========================================`n" -ForegroundColor Cyan

# Deploy kernel
& "$PSScriptRoot\send_ymodem.ps1" `
    -PortName $PortName `
    -BaudRate $BaudRate `
    -FilePath $KernelPath `
    -LoadAddress 0x02080000 `
    -AutoBoot `
    -KernelSector 500000 `
    -KernelSectorCount $SectorCount

# Wait for emergency beacons on high baud rate
Write-Host "[*] Waiting for emergency beacons on $BaudRate baud..." -ForegroundColor Yellow
Start-Sleep -Seconds 3

# Read beacons on high baud rate
$portHigh = New-Object System.IO.Ports.SerialPort $PortName, $BaudRate, None, 8, One
$portHigh.Handshake = [System.IO.Ports.Handshake]::None
$portHigh.ReadTimeout = 3000
$portHigh.WriteTimeout = 1000

try {
    $portHigh.Open()
    $buffer = ""
    $timeout = [DateTime]::Now.AddSeconds(5)
    
    while ([DateTime]::Now -lt $timeout) {
        if ($portHigh.BytesToRead -gt 0) {
            $data = $portHigh.ReadExisting()
            $buffer += $data
            Write-Host $data -NoNewline
            
            # Look for beacons
            if ($buffer -match "ABCDE") {
                Write-Host "`n[+] Emergency beacons detected!" -ForegroundColor Green
                break
            }
        }
        Start-Sleep -Milliseconds 50
    }
    
    $portHigh.Close()
} catch {
    Write-Host "[WARN] Could not read beacons: $($_.Exception.Message)" -ForegroundColor Yellow
} finally {
    if ($portHigh.IsOpen) { $portHigh.Close() }
}

# Switch to 115200 for menu interaction
Write-Host "[*] Switching to 115200 baud for menu..." -ForegroundColor Yellow
Start-Sleep -Milliseconds 500

$port = New-Object System.IO.Ports.SerialPort $PortName, 115200, None, 8, One
$port.Handshake = [System.IO.Ports.Handshake]::None
$port.ReadTimeout = 5000
$port.WriteTimeout = 1000

try {
    $port.Open()
    Write-Host "[*] Serial port opened at 115200 baud" -ForegroundColor Green
    
    # Wait for menu
    Start-Sleep -Seconds 2
    
    # Select heartbeat mode (option 2)
    Write-Host "[*] Selecting Heartbeat Mode..." -ForegroundColor Yellow
    $port.Write("2")
    Start-Sleep -Milliseconds 500
    
    # Collect heartbeat data
    Write-Host "[*] Collecting heartbeat data for $DurationSeconds seconds..." -ForegroundColor Yellow
    Write-Host ""
    
    $beats = @()
    $startTime = [DateTime]::Now
    $lastBeatTime = $null
    $buffer = ""
    
    while (([DateTime]::Now - $startTime).TotalSeconds -lt $DurationSeconds) {
        if ($port.BytesToRead -gt 0) {
            $data = $port.ReadExisting()
            $buffer += $data
            Write-Host $data -NoNewline
            
            # Parse BEAT lines
            $lines = $buffer -split "`r`n"
            foreach ($line in $lines) {
                if ($line -match "^BEAT\s+0x([0-9A-F]+)\s+\|\s+Cycles:\s+0x([0-9A-F]+)\s+\|\s+Jitter:\s+0x([0-9A-F]+)%") {
                    $beatNum = [Convert]::ToInt64($matches[1], 16)
                    $cycles = [Convert]::ToInt64($matches[2], 16)
                    $jitter = [Convert]::ToInt32($matches[3], 16)
                    
                    $beatTime = [DateTime]::Now
                    
                    $beat = [PSCustomObject]@{
                        Number = $beatNum
                        Cycles = $cycles
                        Jitter = $jitter
                        Timestamp = $beatTime
                    }
                    
                    if ($lastBeatTime) {
                        $beat | Add-Member -NotePropertyName "IntervalMs" -NotePropertyValue (($beatTime - $lastBeatTime).TotalMilliseconds)
                    }
                    
                    $beats += $beat
                    $lastBeatTime = $beatTime
                }
            }
            $buffer = $lines[-1]
        }
        Start-Sleep -Milliseconds 10
    }
    
    # Send Ctrl+C to stop heartbeat
    Write-Host "`n`n[*] Stopping heartbeat..." -ForegroundColor Yellow
    $port.Write([char]3)
    Start-Sleep -Seconds 2
    
    # Read final statistics
    if ($port.BytesToRead -gt 0) {
        $finalOutput = $port.ReadExisting()
        Write-Host $finalOutput
    }
    
    # Analyze results
    Write-Host "`n========================================" -ForegroundColor Cyan
    Write-Host "  Heartbeat Analysis" -ForegroundColor Cyan
    Write-Host "========================================`n" -ForegroundColor Cyan
    
    if ($beats.Count -gt 0) {
        $totalBeats = $beats.Count
        $avgCycles = ($beats | Measure-Object -Property Cycles -Average).Average
        $minCycles = ($beats | Measure-Object -Property Cycles -Minimum).Minimum
        $maxCycles = ($beats | Measure-Object -Property Cycles -Maximum).Maximum
        $maxJitter = ($beats | Measure-Object -Property Jitter -Maximum).Maximum
        
        Write-Host "Total beats collected: $totalBeats" -ForegroundColor Green
        Write-Host "Average interval: $([Math]::Round($avgCycles)) cycles" -ForegroundColor Green
        Write-Host "Min interval: $minCycles cycles" -ForegroundColor Green
        Write-Host "Max interval: $maxCycles cycles" -ForegroundColor Green
        Write-Host "Max jitter: $maxJitter%" -ForegroundColor $(if ($maxJitter -gt 5) { "Red" } else { "Green" })
        
        # Calculate interval stability
        $beatsWithInterval = $beats | Where-Object { $_.IntervalMs }
        if ($beatsWithInterval.Count -gt 0) {
            $avgIntervalMs = ($beatsWithInterval | Measure-Object -Property IntervalMs -Average).Average
            $minIntervalMs = ($beatsWithInterval | Measure-Object -Property IntervalMs -Minimum).Minimum
            $maxIntervalMs = ($beatsWithInterval | Measure-Object -Property IntervalMs -Maximum).Maximum
            
            Write-Host ""
            Write-Host "Wall-clock intervals:" -ForegroundColor Yellow
            Write-Host "  Average: $([Math]::Round($avgIntervalMs, 2)) ms" -ForegroundColor Green
            Write-Host "  Min: $([Math]::Round($minIntervalMs, 2)) ms" -ForegroundColor Green
            Write-Host "  Max: $([Math]::Round($maxIntervalMs, 2)) ms" -ForegroundColor Green
            
            $deviation = (($maxIntervalMs - $minIntervalMs) / $avgIntervalMs) * 100
            Write-Host "  Deviation: $([Math]::Round($deviation, 2))%" -ForegroundColor $(if ($deviation -gt 10) { "Red" } else { "Green" })
        }
        
        # Test verdict
        Write-Host ""
        if ($maxJitter -le 5 -and $totalBeats -ge ($DurationSeconds * 10 * 0.9)) {
            Write-Host "[PASS] Heartbeat stability test PASSED" -ForegroundColor Green
            Write-Host "  - Jitter within acceptable range (<= 5%)" -ForegroundColor Green
            Write-Host "  - Beat count consistent with duration" -ForegroundColor Green
        } else {
            Write-Host "[FAIL] Heartbeat stability test FAILED" -ForegroundColor Red
            if ($maxJitter -gt 5) {
                Write-Host "  - Excessive jitter detected: $maxJitter%" -ForegroundColor Red
            }
            if ($totalBeats -lt ($DurationSeconds * 10 * 0.9)) {
                Write-Host "  - Missing beats detected" -ForegroundColor Red
            }
        }
        
        # Save results to JSON
        $results = @{
            timestamp = (Get-Date -Format "yyyy-MM-dd HH:mm:ss")
            duration_seconds = $DurationSeconds
            total_beats = $totalBeats
            avg_cycles = [Math]::Round($avgCycles)
            min_cycles = $minCycles
            max_cycles = $maxCycles
            max_jitter_percent = $maxJitter
            beats = $beats
        }
        
        $resultsFile = "heartbeat_results_$(Get-Date -Format 'yyyyMMdd_HHmmss').json"
        $results | ConvertTo-Json -Depth 10 | Out-File $resultsFile
        Write-Host ""
        Write-Host "[*] Results saved to: $resultsFile" -ForegroundColor Yellow
        
    } else {
        Write-Host "[ERROR] No heartbeat data collected!" -ForegroundColor Red
    }
    
} catch {
    Write-Host "[ERROR] $($_.Exception.Message)" -ForegroundColor Red
} finally {
    if ($port.IsOpen) {
        $port.Close()
    }
}

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "[*] Heartbeat test completed" -ForegroundColor Yellow
Write-Host "========================================`n" -ForegroundColor Cyan
