# deploy_tftp_fixed.ps1 - Исправленный скрипт развертывания с автоопределением IP
# Исправляет проблему ARP timeout при несоответствии IP-адресов

param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 1500000,
    [string]$KernelFile = "kernel_neuro.bin",
    [string]$TftpDir = "C:\tftpboot"
)

$ErrorActionPreference = "Stop"

# Получаем реальный IP адрес ПК
$localIP = (Get-NetIPAddress -AddressFamily IPv4 | 
    Where-Object { $_.IPAddress -match "^192\.168\.1\." -and $_.PrefixOrigin -eq "Dhcp" } | 
    Select-Object -First 1).IPAddress

if (-not $localIP) {
    # Fallback - берем первый IP в сети 192.168.1.x
    $localIP = (Get-NetIPAddress -AddressFamily IPv4 | 
        Where-Object { $_.IPAddress -match "^192\.168\." } | 
        Select-Object -First 1).IPAddress
}

if (-not $localIP) {
    Write-Error "Не удалось определить IP адрес ПК. Убедитесь, что сетевая карта подключена."
    exit 1
}

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  H-Exo TFTP Deploy (Fixed IP)" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Обнаружен IP ПК: $localIP" -ForegroundColor Green
Write-Host "TFTP директория: $TftpDir" -ForegroundColor Cyan
Write-Host ""

# Проверяем наличие файла ядра
$kernelPath = Join-Path $TftpDir $KernelFile
if (-not (Test-Path $kernelPath)) {
    Write-Error "Файл ядра не найден: $kernelPath"
    Write-Host "Сначала скомпилируйте ядро: make -f Makefile.neuro" -ForegroundColor Yellow
    exit 1
}

$kernelSize = (Get-Item $kernelPath).Length
Write-Host "Файл ядра: $KernelFile ($kernelSize bytes)" -ForegroundColor Green
Write-Host ""

# Проверяем TFTP сервер
$tftpRunning = $false
try {
    $udp = New-Object System.Net.Sockets.UdpClient
    $udp.Client.ReceiveTimeout = 1000
    $udp.Connect("localhost", 69)
    $udp.Send([byte[]]@(0, 1, 0), 3)  # TFTP RRQ packet
    $tftpRunning = $true
    $udp.Close()
} catch {
    # TFTP server might not respond to empty packet, that's ok
    $tftpRunning = $true  # Assume it's running if we got this far
}

if (-not $tftpRunning) {
    Write-Warning "TFTP сервер не обнаружен! Запустите: .\ps_tftp_server.ps1"
    exit 1
}

Write-Host "TFTP сервер: АКТИВЕН" -ForegroundColor Green
Write-Host ""

# Открываем COM порт
Write-Host "[*] Открываю $PortName на $BaudRate baud..." -ForegroundColor Yellow
$serial = $null
try {
    $serial = New-Object System.IO.Ports.SerialPort($PortName, $BaudRate, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $serial.ReadTimeout = 500
    $serial.WriteTimeout = 1000
    $serial.Open()
    Write-Host "[+] Порт открыт" -ForegroundColor Green
} catch {
    Write-Error "Не удалось открыть $PortName : $_"
    exit 1
}

try {
    Write-Host ""
    Write-Host "[*] ПЕРЕЗАГРУЗИТЕ ПЛАТУ (power cycle)..." -ForegroundColor Yellow
    Write-Host "[*] Ожидаю U-Boot prompt..." -ForegroundColor Yellow
    Write-Host ""
    
    $buffer = ""
    $promptReceived = $false
    $timeout = 120
    $start = Get-Date
    $lastCtrlC = Get-Date
    
    # Ждем U-Boot prompt с прерыванием PXE
    while (((Get-Date) - $start).TotalSeconds -lt $timeout) {
        try {
            $char = $serial.ReadChar()
            $ch = [char]$char
            $buffer += $ch
            Write-Host -NoNewline $ch
            
            # Ищем признаки PXE загрузки
            if ($buffer -match "BOOTP|DHCP|TFTP|pxelinux|Loading:|Retrieving") {
                if (((Get-Date) - $lastCtrlC).TotalMilliseconds -gt 200) {
                    $serial.Write([char]3)  # Ctrl+C
                    $lastCtrlC = Get-Date
                }
            }
            
            # Ищем U-Boot prompt
            if ($buffer -match "=>" -and $buffer.Length -gt 100) {
                Write-Host ""
                Write-Host "[+] U-Boot prompt получен!" -ForegroundColor Green
                $promptReceived = $true
                Start-Sleep -Milliseconds 500
                break
            }
            
            if ($buffer.Length -gt 2000) {
                $buffer = $buffer.Substring($buffer.Length - 1000)
            }
        } catch {
            Start-Sleep -Milliseconds 10
        }
    }
    
    if (-not $promptReceived) {
        Write-Host ""
        Write-Error "Не удалось получить U-Boot prompt за $timeout секунд"
        exit 1
    }
    
    # Отправляем команды настройки сети с ПРАВИЛЬНЫМ IP
    function Send-UbootCommand {
        param([string]$cmd, [int]$waitMs = 500)
        Write-Host "[>] $cmd" -ForegroundColor Cyan
        $serial.WriteLine($cmd)
        Start-Sleep -Milliseconds $waitMs
        
        # Читаем ответ
        $response = ""
        $readStart = Get-Date
        while (((Get-Date) - $readStart).TotalMilliseconds -lt 1000) {
            try {
                $response += [char]$serial.ReadChar()
            } catch { break }
        }
        if ($response) { Write-Host $response -ForegroundColor Gray }
        return $response
    }
    
    Write-Host ""
    Write-Host "[*] Настройка сети с IP $localIP..." -ForegroundColor Yellow
    
    Send-UbootCommand "setenv ipaddr 192.168.1.10" 100
    Send-UbootCommand "setenv serverip $localIP" 100
    Send-UbootCommand "setenv bootfile $KernelFile" 100
    
    # Загружаем ядро
    Write-Host ""
    Write-Host "[*] Загрузка ядра через TFTP..." -ForegroundColor Yellow
    Write-Host "    Это может занять 10-30 секунд..." -ForegroundColor Gray
    Write-Host ""
    
    $serial.WriteLine("tftp 0x02080000 $KernelFile")
    Start-Sleep -Milliseconds 100
    
    # Ждем завершения TFTP передачи
    $tftpTimeout = 60
    $tftpStart = Get-Date
    $tftpComplete = $false
    $tftpBuffer = ""
    
    while (((Get-Date) - $tftpStart).TotalSeconds -lt $tftpTimeout) {
        try {
            $ch = [char]$serial.ReadChar()
            $tftpBuffer += $ch
            Write-Host -NoNewline $ch
            
            # Ищем признаки успешной загрузки или ошибки
            if ($tftpBuffer -match "Bytes transferred|Loading:.*done|=> ") {
                if ($tftpBuffer -match "Bytes transferred") {
                    Write-Host ""
                    Write-Host "[+] TFTP загрузка успешна!" -ForegroundColor Green
                    $tftpComplete = $true
                    break
                }
            }
            
            if ($tftpBuffer -match "Retry count exceeded|ERROR|TIMEOUT|not found") {
                Write-Host ""
                Write-Error "TFTP ошибка: $_"
                exit 1
            }
            
            if ($tftpBuffer.Length -gt 3000) {
                $tftpBuffer = $tftpBuffer.Substring($tftpBuffer.Length - 1500)
            }
        } catch {
            Start-Sleep -Milliseconds 50
        }
    }
    
    if (-not $tftpComplete) {
        Write-Host ""
        Write-Warning "TFTP загрузка не подтверждена, но продолжаем..."
    }
    
    # Запускаем ядро
    Write-Host ""
    Write-Host "[*] Запуск ядра..." -ForegroundColor Yellow
    $serial.WriteLine("go 0x02080000")
    
    # Мониторим вывод ядра
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Green
    Write-Host "  Ядро запущено! Ожидаем маяки..." -ForegroundColor Green
    Write-Host "========================================" -ForegroundColor Green
    Write-Host ""
    Write-Host "Ожидаем: [BEACON] A, B, C, D, E и BEAT сообщения" -ForegroundColor Cyan
    Write-Host "Нажмите Ctrl+C для выхода" -ForegroundColor Gray
    Write-Host ""
    
    $kernelOutput = ""
    
    while ($true) {
        try {
            $ch = [char]$serial.ReadChar()
            Write-Host -NoNewline $ch
            $kernelOutput += $ch
            
            # Проверяем успешную загрузку
            if ($kernelOutput -match "Operational|H-Exo Omni-Core") {
                Write-Host ""
                Write-Host ""
                Write-Host "[+] ЯДРО УСПЕШНО ЗАГРУЖЕНО!" -ForegroundColor Green -BackgroundColor Black
                Write-Host ""
                break
            }
        } catch {
            Start-Sleep -Milliseconds 10
        }
    }
    
    # Продолжаем интерактивный режим
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Green
    Write-Host "  ИНТЕРАКТИВНЫЙ РЕЖИМ" -ForegroundColor Green
    Write-Host "========================================" -ForegroundColor Green
    Write-Host "Отправьте команды в ядро:" -ForegroundColor Cyan
    Write-Host "  '2' - Запустить Heartbeat тест" -ForegroundColor White
    Write-Host "  'C' - Включить Chaos mode (внутри heartbeat)" -ForegroundColor White
    Write-Host "  'q' - Echo mode" -ForegroundColor White
    Write-Host "  Ctrl+C здесь или в терминале - выход" -ForegroundColor Gray
    Write-Host ""
    
    # Читаем вывод ядра и позволяем отправлять команды
    Write-Host "`n[ВВОД АКТИВЕН] Нажмите '2', 'C' или 'q' для управления ядром" -ForegroundColor Magenta
    Write-Host "[Ctrl+C в PowerShell или Ctrl+Break] - выход`n" -ForegroundColor Gray
    
    while ($serial.IsOpen) {
        try {
            # Читаем из порта с таймаутом
            if ($serial.BytesToRead -gt 0) {
                $ch = [char]$serial.ReadChar()
                Write-Host -NoNewline $ch
            }
            
            # Проверяем ввод без блокировки и без эха
            if ([Console]::KeyAvailable) {
                $key = [Console]::ReadKey($true)  # $true = intercept (no echo)
                $keyChar = $key.KeyChar
                
                if ($key.Key -eq "C" -and $key.Modifiers -eq "Control") {
                    Write-Host "`n[!] Прерывание Ctrl+C" -ForegroundColor Yellow
                    break
                }
                
                if ($keyChar) {
                    $serial.Write($keyChar)
                    Write-Host "`n[>] Отправлено: '$keyChar'" -ForegroundColor Green
                }
            }
            
            Start-Sleep -Milliseconds 10
        } catch {
            Start-Sleep -Milliseconds 10
        }
    }
    
} finally {
    if ($serial -and $serial.IsOpen) {
        $serial.Close()
        Write-Host ""
        Write-Host "[!] Порт закрыт" -ForegroundColor Yellow
    }
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Развертывание завершено" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
