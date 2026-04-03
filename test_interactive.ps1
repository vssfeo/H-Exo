param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 115200
)

$port = New-Object System.IO.Ports.SerialPort $PortName, $BaudRate
$port.ReadTimeout = 500

try {
    $port.Open()
    Write-Host "[+] Port opened. Sending test commands..." -ForegroundColor Green
    
    # Отправляем Enter несколько раз
    for ($i = 0; $i -lt 5; $i++) {
        $port.WriteLine("")
        Start-Sleep -Milliseconds 200
        
        while ($port.BytesToRead -gt 0) {
            $b = $port.ReadByte()
            if ($b -ge 0) {
                Write-Host -NoNewline ([char]$b) -ForegroundColor Cyan
            }
        }
    }
    
    Write-Host "`n[*] Sending 'help' command..." -ForegroundColor Yellow
    $port.WriteLine("help")
    Start-Sleep -Milliseconds 500
    
    while ($port.BytesToRead -gt 0) {
        $b = $port.ReadByte()
        if ($b -ge 0) {
            Write-Host -NoNewline ([char]$b) -ForegroundColor Cyan
        }
    }
    
    Write-Host "`n[*] Done" -ForegroundColor Yellow
    
} catch {
    Write-Host "[!] Error: $_" -ForegroundColor Red
} finally {
    if ($port.IsOpen) {
        $port.Close()
    }
}
