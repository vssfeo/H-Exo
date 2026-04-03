# Continuous COM3 listener - run this and then power cycle board
param(
    [int]$BaudRate = 1500000
)

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Continuous COM3 Listener" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "[*] Baud: $BaudRate" -ForegroundColor Yellow
Write-Host "[*] Press Ctrl+C to stop" -ForegroundColor Yellow
Write-Host ""
Write-Host "POWER CYCLE THE BOARD NOW!" -ForegroundColor Red
Write-Host ""

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
    $port.ReadTimeout = 500
    $port.WriteTimeout = 500
    $port.DtrEnable = $true
    $port.RtsEnable = $true
    
    $port.Open()
    Write-Host "[+] COM3 opened at $BaudRate baud" -ForegroundColor Green
    Write-Host "[*] Listening... (any output will appear below)" -ForegroundColor Yellow
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host ""
    
    $port.DiscardInBuffer()
    $port.DiscardOutBuffer()
    
    $totalBytes = 0
    $lastEnter = [DateTime]::Now
    
    while ($true) {
        # Send Enter every 3 seconds to provoke response
        if (([DateTime]::Now - $lastEnter).TotalSeconds -ge 3) {
            $port.Write([char]13)
            $lastEnter = [DateTime]::Now
        }
        
        if ($port.BytesToRead -gt 0) {
            $chunk = $port.ReadExisting()
            $totalBytes += $chunk.Length
            Write-Host $chunk -NoNewline -ForegroundColor White
            
            # If we got data, show stats every 1000 bytes
            if ($totalBytes % 1000 -lt $chunk.Length) {
                Write-Host "`n[Stats: $totalBytes bytes received]" -ForegroundColor Gray
            }
        }
        
        Start-Sleep -Milliseconds 50
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
