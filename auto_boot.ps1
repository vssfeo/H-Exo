param(
    [string]$PortName = "COM3",
    [string]$KernelPath = "C:\Users\SERYOGA\AndroidStudioProjects\H-Exo\kernel.bin",
    [UInt32]$LoadAddress = 0x02080000,
    [UInt32]$KernelSector = 32600,
    [UInt32]$KernelSectorCount = 0
)

Write-Host "[*] H-Exo Auto Boot Script" -ForegroundColor Cyan
Write-Host "[*] Waiting for board boot on $PortName..." -ForegroundColor Yellow
Write-Host "[*] Listening at 1,500,000 baud for BootROM, then switching to 115200 for U-Boot" -ForegroundColor Yellow
Write-Host ""

# Открываем порт на скорости BootROM
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
    
    $buffer = New-Object System.Text.StringBuilder
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $ubootDetected = $false
    $switchedTo115200 = $false
    
    Write-Host "[*] Waiting for boot messages... (Press Ctrl+C to abort)" -ForegroundColor Yellow
    Write-Host "----------------------------------------" -ForegroundColor Cyan
    
    while ($sw.Elapsed.TotalSeconds -lt 120) {
        try {
            while ($port.BytesToRead -gt 0) {
                $b = $port.ReadByte()
                if ($b -ge 0) {
                    $ch = [char]$b
                    [void]$buffer.Append($ch)
                    Write-Host -NoNewline $ch -ForegroundColor Gray
                }
            }
            
            $text = $buffer.ToString()
            
            # Детект переключения на 115200 (U-Boot обычно выводит что-то после переключения)
            if (-not $switchedTo115200 -and ($text -match 'U-Boot' -or $text.Length -gt 200)) {
                Write-Host "`n[*] Detected U-Boot messages, switching to 115200 baud..." -ForegroundColor Yellow
                $port.Close()
                Start-Sleep -Milliseconds 500
                
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
                $switchedTo115200 = $true
                $buffer.Clear()
                continue
            }
            
            # Детект U-Boot prompt
            if ($switchedTo115200 -and ($text -match '=>' -or $text -match 'Hit any key to stop autoboot')) {
                Write-Host "`n[+] U-Boot prompt detected!" -ForegroundColor Green
                $ubootDetected = $true
                break
            }
            
            # Отправляем Ctrl+C для прерывания autoboot (только на 115200)
            if ($switchedTo115200 -and $sw.ElapsedMilliseconds % 500 -lt 50) {
                $port.Write([char]0x03)
            }
            
        } catch {
            # Timeout - нормально
        }
        Start-Sleep -Milliseconds 50
    }
    
    Write-Host "----------------------------------------" -ForegroundColor Cyan
    
    if (-not $ubootDetected) {
        Write-Host "[!] U-Boot prompt not detected. Check board connection." -ForegroundColor Red
        exit 1
    }
    
    # Теперь загружаем kernel через YMODEM
    Write-Host "[*] Calling send_ymodem.ps1 for kernel transfer..." -ForegroundColor Yellow
    $port.Close()
    
    & "$PSScriptRoot\send_ymodem.ps1" -PortName $PortName -BaudRate 115200 -FilePath $KernelPath -LoadAddress $LoadAddress -AutoBoot -KernelSector $KernelSector -KernelSectorCount $KernelSectorCount
    
} catch {
    Write-Host "[!] Error: $_" -ForegroundColor Red
    exit 1
} finally {
    if ($port.IsOpen) {
        $port.Close()
    }
}
