param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 1500000,
    [string]$KernelPath = "C:\Users\SERYOGA\AndroidStudioProjects\H-Exo\kernel.bin",
    [UInt32]$LoadAddress = 0x02080000,
    [UInt32]$KernelSector = 32600,
    [UInt32]$KernelSectorCount = 3
)

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  H-Exo Automatic Boot & Test System" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

if (-not (Test-Path $KernelPath)) {
    Write-Host "[!] kernel.bin not found at: $KernelPath" -ForegroundColor Red
    exit 1
}

$fileSize = (Get-Item $KernelPath).Length
Write-Host "[*] Kernel: $KernelPath ($fileSize bytes)" -ForegroundColor Yellow
Write-Host "[*] Port: $PortName at $BaudRate baud" -ForegroundColor Yellow
Write-Host "[*] Load address: 0x$($LoadAddress.ToString('X8'))" -ForegroundColor Yellow
Write-Host "[*] SD sector: $KernelSector (count: $KernelSectorCount)" -ForegroundColor Yellow
Write-Host ""

# Открываем порт
$port = New-Object System.IO.Ports.SerialPort $PortName, $BaudRate
$port.Parity = [System.IO.Ports.Parity]::None
$port.DataBits = 8
$port.StopBits = [System.IO.Ports.StopBits]::One
$port.Handshake = [System.IO.Ports.Handshake]::None
$port.ReadTimeout = 100
$port.DtrEnable = $false
$port.RtsEnable = $false

function Read-UntilPrompt {
    param([System.IO.Ports.SerialPort]$Port, [int]$TimeoutSeconds = 30)
    
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $buffer = New-Object System.Text.StringBuilder
    
    while ($sw.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        try {
            while ($Port.BytesToRead -gt 0) {
                $b = $Port.ReadByte()
                if ($b -ge 0) {
                    $ch = [char]$b
                    [void]$buffer.Append($ch)
                    Write-Host -NoNewline $ch -ForegroundColor Gray
                }
            }
            
            $text = $buffer.ToString()
            if ($text -match '=>') {
                return $true
            }
            
            # Отправляем Ctrl+C каждые 300ms
            if ($sw.ElapsedMilliseconds % 300 -lt 50) {
                $Port.Write([char]0x03)
            }
        } catch {}
        Start-Sleep -Milliseconds 50
    }
    return $false
}

function Send-Command {
    param([System.IO.Ports.SerialPort]$Port, [string]$Cmd, [int]$ReadMs = 1000)
    
    Write-Host "`n[CMD] $Cmd" -ForegroundColor Yellow
    $Port.WriteLine($Cmd)
    Start-Sleep -Milliseconds 200
    
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $ReadMs) {
        try {
            while ($Port.BytesToRead -gt 0) {
                $b = $Port.ReadByte()
                if ($b -ge 0) {
                    Write-Host -NoNewline ([char]$b) -ForegroundColor Cyan
                }
            }
        } catch {}
        Start-Sleep -Milliseconds 10
    }
    Write-Host ""
}

try {
    $port.Open()
    Write-Host "[+] Port opened successfully" -ForegroundColor Green
    Write-Host "[*] Waiting for U-Boot prompt (reboot board now)..." -ForegroundColor Yellow
    Write-Host "========================================" -ForegroundColor Cyan
    
    if (-not (Read-UntilPrompt -Port $port -TimeoutSeconds 60)) {
        Write-Host "`n[!] U-Boot prompt not detected" -ForegroundColor Red
        exit 1
    }
    
    Write-Host "`n[+] U-Boot prompt detected!" -ForegroundColor Green
    Write-Host "========================================" -ForegroundColor Cyan
    
    # Загружаем kernel через loady
    Write-Host "`n[*] Starting YMODEM transfer..." -ForegroundColor Yellow
    $loadCmd = "loady 0x$($LoadAddress.ToString('X8'))"
    Send-Command -Port $port -Cmd $loadCmd -ReadMs 500
    
    # YMODEM передача (упрощённая версия - просто отправляем файл)
    Write-Host "[*] Sending kernel.bin..." -ForegroundColor Yellow
    $fileBytes = [System.IO.File]::ReadAllBytes($KernelPath)
    
    # Ждём 'C' от loady
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $gotC = $false
    while ($sw.ElapsedMilliseconds -lt 5000) {
        try {
            if ($port.BytesToRead -gt 0) {
                $b = $port.ReadByte()
                Write-Host -NoNewline ([char]$b) -ForegroundColor Gray
                if ($b -eq 0x43) { # 'C'
                    $gotC = $true
                    break
                }
            }
        } catch {}
        Start-Sleep -Milliseconds 10
    }
    
    if (-not $gotC) {
        Write-Host "`n[!] loady did not send 'C' - YMODEM handshake failed" -ForegroundColor Red
        Write-Host "[*] Trying alternative: write kernel to SD manually" -ForegroundColor Yellow
        
        # Альтернатива: используем mw.b для записи в память
        Write-Host "`n[*] Using memory write commands instead..." -ForegroundColor Yellow
        
        # Сбрасываем буфер
        $port.ReadExisting() | Out-Null
        Start-Sleep -Milliseconds 500
        
        # Пишем kernel напрямую на SD
        $writeCmd = "mmc dev 1"
        Send-Command -Port $port -Cmd $writeCmd -ReadMs 1000
        
        Write-Host "[!] Cannot transfer via UART without YMODEM support" -ForegroundColor Red
        Write-Host "[*] Please use write_kernel_raw.ps1 to write kernel to SD card sector $KernelSector" -ForegroundColor Yellow
        Write-Host "[*] Then use these U-Boot commands:" -ForegroundColor Yellow
        Write-Host "    mmc dev 1" -ForegroundColor Cyan
        Write-Host "    mmc read 0x$($LoadAddress.ToString('X8')) $KernelSector $KernelSectorCount" -ForegroundColor Cyan
        Write-Host "    go 0x$($LoadAddress.ToString('X8'))" -ForegroundColor Cyan
        
        # Пробуем загрузить с SD
        Write-Host "`n[*] Attempting to load kernel from SD sector $KernelSector..." -ForegroundColor Yellow
        Send-Command -Port $port -Cmd "mmc dev 1" -ReadMs 1000
        Send-Command -Port $port -Cmd "mmc read 0x$($LoadAddress.ToString('X8')) $KernelSector $KernelSectorCount" -ReadMs 1500
        Send-Command -Port $port -Cmd "go 0x$($LoadAddress.ToString('X8'))" -ReadMs 5000
        
        Write-Host "`n[*] Monitoring for H-Exo output..." -ForegroundColor Yellow
        Write-Host "========================================" -ForegroundColor Cyan
        
        # Читаем вывод H-Exo
        $sw.Restart()
        while ($sw.Elapsed.TotalSeconds -lt 10) {
            try {
                while ($port.BytesToRead -gt 0) {
                    $b = $port.ReadByte()
                    if ($b -ge 0) {
                        Write-Host -NoNewline ([char]$b) -ForegroundColor Green
                    }
                }
            } catch {}
            Start-Sleep -Milliseconds 10
        }
        
        Write-Host "`n========================================" -ForegroundColor Cyan
        Write-Host "[*] Boot sequence completed" -ForegroundColor Yellow
        exit 0
    }
    
    Write-Host "`n[+] YMODEM handshake successful" -ForegroundColor Green
    # TODO: Implement full YMODEM protocol here
    
} catch {
    Write-Host "`n[!] Error: $_" -ForegroundColor Red
    exit 1
} finally {
    if ($port.IsOpen) {
        $port.Close()
        Write-Host "[*] Port closed" -ForegroundColor Yellow
    }
}
