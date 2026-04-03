param(
    [string]$PortName = "COM3"
)

Write-Host "[*] Capturing boot sequence on $PortName" -ForegroundColor Cyan
Write-Host "[*] Starting at 1,500,000 baud..." -ForegroundColor Yellow
Write-Host ""

# Пробуем на 1,500,000
$port = New-Object System.IO.Ports.SerialPort $PortName, 1500000
$port.Parity = [System.IO.Ports.Parity]::None
$port.DataBits = 8
$port.StopBits = [System.IO.Ports.StopBits]::One
$port.Handshake = [System.IO.Ports.Handshake]::None
$port.ReadTimeout = 100
$port.DtrEnable = $false
$port.RtsEnable = $false

try {
    $port.Open()
    Write-Host "[+] Port opened at 1,500,000 baud" -ForegroundColor Green
    Write-Host "========== 1,500,000 BAUD ==========" -ForegroundColor Cyan
    
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $bytesAt1500k = 0
    $buffer = New-Object System.Text.StringBuilder
    
    # Читаем 30 секунд на высокой скорости и ищем U-Boot prompt
    while ($sw.Elapsed.TotalSeconds -lt 30) {
        try {
            while ($port.BytesToRead -gt 0) {
                $b = $port.ReadByte()
                if ($b -ge 0) {
                    $bytesAt1500k++
                    $ch = [char]$b
                    [void]$buffer.Append($ch)
                    Write-Host -NoNewline $ch -ForegroundColor Gray
                }
            }
            
            $text = $buffer.ToString()
            if ($text -match '=>' -or $text -match 'Hit any key to stop autoboot') {
                Write-Host "`n`n[+] U-Boot prompt detected at 1,500,000 baud!" -ForegroundColor Green
                Write-Host "[*] U-Boot is using 1,500,000 baud, not 115200" -ForegroundColor Yellow
                return
            }
            
            # Отправляем Ctrl+C каждые 500ms для прерывания autoboot
            if ($sw.ElapsedMilliseconds % 500 -lt 50) {
                $port.Write([char]0x03)
            }
            
        } catch {}
        Start-Sleep -Milliseconds 10
    }
    
    Write-Host "`n========== SWITCHING TO 115200 ==========" -ForegroundColor Cyan
    Write-Host "[*] Received $bytesAt1500k bytes at 1,500,000 baud" -ForegroundColor Yellow
    
    $port.Close()
    Start-Sleep -Milliseconds 500
    
    # Переключаемся на 115200
    $port = New-Object System.IO.Ports.SerialPort $PortName, 115200
    $port.Parity = [System.IO.Ports.Parity]::None
    $port.DataBits = 8
    $port.StopBits = [System.IO.Ports.StopBits]::One
    $port.Handshake = [System.IO.Ports.Handshake]::None
    $port.ReadTimeout = 100
    $port.DtrEnable = $false
    $port.RtsEnable = $false
    $port.Open()
    
    Write-Host "[+] Switched to 115200 baud" -ForegroundColor Green
    Write-Host "========== 115200 BAUD ==========" -ForegroundColor Cyan
    
    $sw.Restart()
    $buffer = New-Object System.Text.StringBuilder
    
    # Читаем на 115200 и ищем U-Boot prompt
    while ($sw.Elapsed.TotalSeconds -lt 30) {
        try {
            while ($port.BytesToRead -gt 0) {
                $b = $port.ReadByte()
                if ($b -ge 0) {
                    $ch = [char]$b
                    [void]$buffer.Append($ch)
                    Write-Host -NoNewline $ch -ForegroundColor Green
                }
            }
            
            $text = $buffer.ToString()
            if ($text -match '=>') {
                Write-Host "`n`n[+] U-Boot prompt detected!" -ForegroundColor Green
                Write-Host "[*] Ready for commands" -ForegroundColor Yellow
                break
            }
            
            # Отправляем Ctrl+C каждые 500ms
            if ($sw.ElapsedMilliseconds % 500 -lt 50) {
                $port.Write([char]0x03)
            }
            
        } catch {}
        Start-Sleep -Milliseconds 50
    }
    
    Write-Host "`n========== DONE ==========" -ForegroundColor Cyan
    
} catch {
    Write-Host "[!] Error: $_" -ForegroundColor Red
} finally {
    if ($port.IsOpen) {
        $port.Close()
    }
}
