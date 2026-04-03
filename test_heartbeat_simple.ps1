param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 115200
)

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  H-Exo Heartbeat Test (Simplified)" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

Write-Host "[*] Instructions:" -ForegroundColor Yellow
Write-Host "    1. Manually flash kernel_neuro.bin to SD sector 500000" -ForegroundColor Gray
Write-Host "    2. In U-Boot, run: mmc dev 1; mmc read 0x02080000 500000 40; go 0x02080000" -ForegroundColor Gray
Write-Host "    3. Select option 2 (Heartbeat Mode)" -ForegroundColor Gray
Write-Host ""
Write-Host "[*] Opening serial port at $BaudRate baud..." -ForegroundColor Yellow

$port = New-Object System.IO.Ports.SerialPort $PortName, $BaudRate, None, 8, One
$port.Handshake = [System.IO.Ports.Handshake]::None
$port.ReadTimeout = 1000
$port.WriteTimeout = 1000

try {
    $port.Open()
    Write-Host "[+] Serial port opened" -ForegroundColor Green
    Write-Host "[*] Monitoring output (Ctrl+C to stop)..." -ForegroundColor Yellow
    Write-Host ""
    
    $beats = @()
    $startTime = $null
    
    while ($true) {
        if ($port.BytesToRead -gt 0) {
            $data = $port.ReadExisting()
            Write-Host $data -NoNewline
            
            # Parse BEAT lines
            if ($data -match "BEAT\s+0x([0-9A-F]+)") {
                $beatNum = [Convert]::ToInt64($matches[1], 16)
                $beatTime = [DateTime]::Now
                
                if ($null -eq $startTime) {
                    $startTime = $beatTime
                }
                
                $beats += [PSCustomObject]@{
                    Number = $beatNum
                    Time = $beatTime
                    Elapsed = ($beatTime - $startTime).TotalSeconds
                }
                
                Write-Host "[BEAT $beatNum detected at $($beats[-1].Elapsed)s]" -ForegroundColor Green
            }
        }
        Start-Sleep -Milliseconds 50
    }
} catch {
    Write-Host "`n[ERROR] $($_.Exception.Message)" -ForegroundColor Red
} finally {
    if ($port.IsOpen) {
        $port.Close()
    }
    
    if ($beats.Count -gt 0) {
        Write-Host "`n========================================" -ForegroundColor Cyan
        Write-Host "  Heartbeat Summary" -ForegroundColor Cyan
        Write-Host "========================================" -ForegroundColor Cyan
        Write-Host "Total beats: $($beats.Count)" -ForegroundColor Green
        Write-Host "Duration: $([Math]::Round(($beats[-1].Time - $beats[0].Time).TotalSeconds, 2))s" -ForegroundColor Green
    }
}
