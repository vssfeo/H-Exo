# Raw COM3 listener for diagnostics
param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 1500000,
    [int]$ListenSeconds = 60
)

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  COM3 Raw Listener (Diagnostics)" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "[*] Port: $PortName" -ForegroundColor Yellow
Write-Host "[*] Baud: $BaudRate" -ForegroundColor Yellow
Write-Host "[*] Duration: $ListenSeconds seconds" -ForegroundColor Yellow
Write-Host ""

# Kill any process holding COM3
$comProcesses = Get-CimInstance Win32_PnPEntity | Where-Object {
    $_.Name -match "COM3"
} | ForEach-Object {
    $comPort = $_.Name
    Get-Process | Where-Object {
        $_.MainWindowTitle -match "COM3" -or $_.ProcessName -match "powershell"
    }
} | Select-Object -Unique

if ($comProcesses) {
    Write-Host "[!] Killing processes that may hold COM3..." -ForegroundColor Yellow
    Get-Process | Where-Object {
        $_.ProcessName -eq "pwsh" -or $_.ProcessName -eq "powershell"
    } | Where-Object {
        $_.Id -ne $PID
    } | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
}

try {
    $port = New-Object System.IO.Ports.SerialPort
    $port.PortName = $PortName
    $port.BaudRate = $BaudRate
    $port.DataBits = 8
    $port.Parity = [System.IO.Ports.Parity]::None
    $port.StopBits = [System.IO.Ports.StopBits]::One
    $port.Handshake = [System.IO.Ports.Handshake]::None
    $port.ReadTimeout = 500
    $port.WriteTimeout = 500

    Write-Host "[*] Opening $PortName..." -ForegroundColor Yellow
    $port.Open()
    Write-Host "[+] Port opened successfully." -ForegroundColor Green
    Write-Host ""
    Write-Host "=== LISTENING (send Enter every 2 sec) ===" -ForegroundColor Cyan
    Write-Host ""

    $port.DiscardInBuffer()
    $port.DiscardOutBuffer()

    $deadline = [DateTime]::Now.AddSeconds($ListenSeconds)
    $lastEnter = [DateTime]::MinValue
    $byteCount = 0

    while ([DateTime]::Now -lt $deadline) {
        # Send Enter periodically to provoke response
        if (([DateTime]::Now - $lastEnter).TotalSeconds -ge 2) {
            $port.Write([char]13)
            $lastEnter = [DateTime]::Now
        }

        if ($port.BytesToRead -gt 0) {
            $chunk = $port.ReadExisting()
            $byteCount += $chunk.Length
            Write-Host $chunk -NoNewline -ForegroundColor White
        }

        Start-Sleep -Milliseconds 50
    }

    Write-Host ""
    Write-Host ""
    Write-Host "=== LISTENING COMPLETE ===" -ForegroundColor Cyan
    Write-Host "[*] Total bytes received: $byteCount" -ForegroundColor Yellow

    if ($byteCount -eq 0) {
        Write-Host "[!] NO DATA RECEIVED. Possible issues:" -ForegroundColor Red
        Write-Host "    1. Board is not powered or not connected to COM3" -ForegroundColor Red
        Write-Host "    2. Wrong baud rate (try 115200 with: -BaudRate 115200)" -ForegroundColor Red
        Write-Host "    3. TX/RX lines swapped or damaged cable" -ForegroundColor Red
        Write-Host "    4. Board is stuck in boot loop or not outputting to UART2" -ForegroundColor Red
    } else {
        Write-Host "[+] Data received successfully. Board is communicating." -ForegroundColor Green
    }

} catch {
    Write-Host "[ERROR] $($_.Exception.Message)" -ForegroundColor Red
} finally {
    if ($port -and $port.IsOpen) {
        $port.Close()
        $port.Dispose()
        Write-Host "[!] Port closed." -ForegroundColor Yellow
    }
}
