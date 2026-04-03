param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 115200,
    [int]$DurationSeconds = 30
)

Write-Host "[*] Opening $PortName at $BaudRate baud..." -ForegroundColor Yellow
Write-Host "[*] Will listen for $DurationSeconds seconds..." -ForegroundColor Yellow
Write-Host "[*] Press Ctrl+C to stop early" -ForegroundColor Yellow
Write-Host ""

$port = New-Object System.IO.Ports.SerialPort $PortName, $BaudRate
$port.Parity = [System.IO.Ports.Parity]::None
$port.DataBits = 8
$port.StopBits = [System.IO.Ports.StopBits]::One
$port.Handshake = [System.IO.Ports.Handshake]::None
$port.ReadTimeout = 100

try {
    $port.Open()
    Write-Host "[+] Port opened successfully!" -ForegroundColor Green
    Write-Host "[*] Waiting for data..." -ForegroundColor Yellow
    Write-Host "----------------------------------------" -ForegroundColor Cyan
    
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $bytesReceived = 0
    
    while ($sw.Elapsed.TotalSeconds -lt $DurationSeconds) {
        try {
            while ($port.BytesToRead -gt 0) {
                $b = $port.ReadByte()
                if ($b -ge 0) {
                    $bytesReceived++
                    Write-Host -NoNewline ([char]$b)
                }
            }
        } catch {
            # Timeout is normal
        }
        Start-Sleep -Milliseconds 10
    }
    
    Write-Host ""
    Write-Host "----------------------------------------" -ForegroundColor Cyan
    Write-Host "[*] Total bytes received: $bytesReceived" -ForegroundColor Yellow
    
    if ($bytesReceived -eq 0) {
        Write-Host "[!] No data received. Check:" -ForegroundColor Red
        Write-Host "    - Board is powered on" -ForegroundColor Red
        Write-Host "    - USB cable is connected" -ForegroundColor Red
        Write-Host "    - Correct COM port ($PortName)" -ForegroundColor Red
        Write-Host "    - Correct baud rate ($BaudRate)" -ForegroundColor Red
    } else {
        Write-Host "[+] Data received successfully!" -ForegroundColor Green
    }
    
} catch {
    Write-Host "[!] Error: $_" -ForegroundColor Red
} finally {
    if ($port.IsOpen) {
        $port.Close()
        Write-Host "[*] Port closed" -ForegroundColor Yellow
    }
}
