# deploy_tftp_fixed.ps1 - Исправленный скрипт развертывания с автоопределением IP
# Исправляет проблему ARP timeout при несоответствии IP-адресов
# Требуется PowerShell 7+ (pwsh). В Windows PowerShell 5.1 разбор этого файла может завершаться ошибкой.
#requires -Version 7.0

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
Write-Host "Подсказка: если на UART застряло на LPDDR3 933MHz failed после прошивки u-boot," -ForegroundColor DarkYellow
Write-Host "  SPL собран не под твою плату (DDR3 vs LPDDR3). Восстанови загрузчик с рабочей SD" -ForegroundColor DarkYellow
Write-Host "  или прошивай idbloader/u-boot.itb, собранные под NanoPi M4 (DDR3)." -ForegroundColor DarkYellow
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
Write-Host "Файл ядра: $KernelFile ($($kernelSize) bytes)" -ForegroundColor Green
Write-Host ""

# Проверяем что TFTP сервер действительно слушает UDP:69.
# Важно: bind() на Any здесь ненадежен, если сервер привязан к конкретному IP.
$udp69 = Get-NetUDPEndpoint -LocalPort 69 -ErrorAction SilentlyContinue
if (-not $udp69) {
    Write-Host "TFTP сервер: не обнаружен на UDP 69, запускаю фоновый сервер..." -ForegroundColor Yellow
    & (Join-Path $PSScriptRoot "start_tftp_server_bg.ps1")
    Start-Sleep -Seconds 2
    $udp69 = Get-NetUDPEndpoint -LocalPort 69 -ErrorAction SilentlyContinue
}

if (-not $udp69) {
    Write-Host "TFTP сервер не запустился. Запусти вручную: .\start_tftp_server_bg.ps1" -ForegroundColor Red
    exit 1
}

Write-Host "TFTP сервер: АКТИВЕН" -ForegroundColor Green
Write-Host ""

# Открываем COM порт
Write-Host "`[*] Открываю $PortName на $BaudRate baud..." -ForegroundColor Yellow
$serial = $null
try {
    $serial = New-Object System.IO.Ports.SerialPort($PortName, $BaudRate, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $serial.ReadTimeout = 500
    $serial.WriteTimeout = 1000
    $serial.Open()
    Write-Host "`[+] Порт открыт" -ForegroundColor Green
} catch {
    Write-Error "Не удалось открыть $PortName : $_"
    exit 1
}

function Send-UbootCommand {
    param(
        $SerialPort,
        [string] $cmd,
        [int] $waitMs = 800
    )
    Write-Host "`[>] $cmd" -ForegroundColor Cyan
    $SerialPort.DiscardInBuffer()
    $SerialPort.Write($cmd + "`r")
    $response = ""
    $readStart = Get-Date
    while (((Get-Date) - $readStart).TotalMilliseconds -lt $waitMs) {
        try {
            $ch = [char]$SerialPort.ReadChar()
            $response += $ch
            if ($response -match '=>\s*$') { break }
        } catch { break }
    }
}


try {
    Write-Host ""
    Write-Host "`[*] Проверяю состояние платы (ядро или U-Boot)..." -ForegroundColor Yellow
    Write-Host ""

    # Step 1: read 3 seconds of UART output (probe)
    $buffer = ""
    $probeEnd = (Get-Date).AddSeconds(3)
    while ((Get-Date) -lt $probeEnd) {
        try {
            $ch = [char]$serial.ReadChar()
            $buffer += $ch
            Write-Host -NoNewline $ch
        } catch { Start-Sleep -Milliseconds 5 }
    }

    # If kernel interactive prompt detected, send soft reboot
    if ($buffer -match '> \s*$' -or $buffer -match '[>]\s*$') {
        Write-Host "" 
        Write-Host "`[*] Обнаружено работающее ядро (> prompt). Отправляю команду reboot r..." -ForegroundColor Yellow
        Start-Sleep -Milliseconds 200
        $serial.DiscardInBuffer()
        $serial.Write("r`r")
        Start-Sleep -Milliseconds 500
        Write-Host "`[*] Reboot отправлен. Ожидаю U-Boot..." -ForegroundColor Yellow
        Write-Host ""
    } else {
        Write-Host ""
        Write-Host "`[*] ПЕРЕЗАГРУЗИТЕ ПЛАТУ (power cycle) если U-Boot не появится через 10 сек..." -ForegroundColor Yellow
        Write-Host ""
    }

    $promptReceived = $false
    $timeout = 120
    $start = Get-Date
    $lastBreak = Get-Date
    $buffer = ""

    # Ждем U-Boot prompt и АГРЕССИВНО срываем autoboot/PXE.
    while (((Get-Date) - $start).TotalSeconds -lt $timeout) {
        if (((Get-Date) - $lastBreak).TotalMilliseconds -gt 120) {
            try {
                $serial.Write(" ")
                $serial.Write("`r")
                $serial.Write([char]3)  # Ctrl+C
            } catch {}
            $lastBreak = Get-Date
        }
        try {
            $char = $serial.ReadChar()
            $ch = [char]$char
            $buffer += $ch
            Write-Host -NoNewline $ch
            
            # Ищем признаки PXE загрузки
            if ($buffer -match "BOOTP|DHCP|TFTP|pxelinux|Loading:|Retrieving|Hit any key to stop autoboot") {
                try { $serial.Write([char]3) } catch {}
            }
            
            # If kernel still running, retry reboot
            if ($buffer -match '> \s*$' -and $buffer.Length -lt 200) {
                try { $serial.Write("r`r") } catch {}
            }

            # Детектируем DRAM init fail (только специфичная строка DDR init failure)
            # Do not use Read-Host here (blocks autoboot interception).
            if ($buffer -match "some channel init fail") {
                Write-Host ""
                Write-Host "`[!] DDR init fail detected - power cycle board NOW!" -ForegroundColor Red -BackgroundColor DarkRed
                $buffer = ""
                $start = Get-Date  # сбросить таймаут, ждём пока юзер передёрнет питание
            }

            # Ищем U-Boot prompt
            if ($buffer -match '=>\s*$') {
                Write-Host ""
                Write-Host "`[+] U-Boot prompt получен!" -ForegroundColor Green
                $promptReceived = $true
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

    # Сбрасываем буфер (остатки PXE + Ctrl+C ответы)
    Start-Sleep -Milliseconds 200
    $serial.DiscardInBuffer()

    # Отправляем команды настройки сети с ПРАВИЛЬНЫМ IP
    Write-Host ""
    Write-Host "`[*] Настройка сети с IP $localIP..." -ForegroundColor Yellow
    
    Send-UbootCommand $serial "setenv ipaddr 192.168.1.10"
    Send-UbootCommand $serial "setenv serverip $localIP"
    Send-UbootCommand $serial "setenv bootfile $KernelFile"

    # NOTE: BL31 RAM override removed: overwriting 0x40000 destroys BL31 runtime
    # data (cpuson_flags, cpuson_entry_point) in the same DRAM region.

    # Загружаем ядро
    Write-Host ""
    Write-Host "`[*] Загрузка ядра через TFTP..." -ForegroundColor Yellow
    Write-Host "    Это может занять 10-30 секунд..." -ForegroundColor Gray
    Write-Host ""
    
    $serial.Write("tftp 0x02080000 $KernelFile`r")
    Start-Sleep -Milliseconds 100
    
    # Ждем завершения TFTP передачи
    $tftpTimeout = 60
    $tftpStart = Get-Date
    $tftpComplete = $false
    $tftpFailed = $false
    $tftpBuffer = ""
    
    while (((Get-Date) - $tftpStart).TotalSeconds -lt $tftpTimeout) {
        try {
            $ch = [char]$serial.ReadChar()
            $tftpBuffer += $ch
            Write-Host -NoNewline $ch
            
            # Успешная загрузка
            if ($tftpBuffer -match "Bytes transferred") {
                Write-Host ""
                Write-Host "`[+] TFTP загрузка успешна!" -ForegroundColor Green
                $tftpComplete = $true
                break
            }
            
            # Ошибка передачи
            if ($tftpBuffer -match "Retry count exceeded") {
                Write-Host ""
                Write-Host "`[!] TFTP FAILED: Retry count exceeded" -ForegroundColor Red
                Write-Host "    Проверьте что ps_tftp_server.ps1 запущен как Administrator" -ForegroundColor Yellow
                $tftpFailed = $true
                break
            }
            
            if ($tftpBuffer -match "Could not initialize PHY|TIMEOUT") {
                Write-Host ""
                Write-Host "`[!] TFTP FAILED: Ethernet PHY error" -ForegroundColor Red
                Write-Host "    Проверьте Ethernet кабель NanoPi M4" -ForegroundColor Yellow
                $tftpFailed = $true
                break
            }
            
            if ($tftpBuffer.Length -gt 3000) {
                $tftpBuffer = $tftpBuffer.Substring($tftpBuffer.Length - 1500)
            }
        } catch {
            Start-Sleep -Milliseconds 50
        }
    }
    
    if ($tftpFailed) {
        Write-Host ""
        Write-Host "`[!] Деплой прерван: TFTP загрузка не удалась" -ForegroundColor Red
        exit 1
    }
    
    if (-not $tftpComplete) {
        Write-Host ""
        Write-Host "`[!] Деплой прерван: TFTP таймаут $($tftpTimeout) секунд" -ForegroundColor Red
        exit 1
    }
    
    # Disable caches before kernel launch (matches booti cleanup_before_linux)
    Write-Host ""
    Write-Host "`[*] Сброс кэшей (dcache/icache off)..." -ForegroundColor Yellow
    Send-UbootCommand $serial "dcache off" 1500
    Send-UbootCommand $serial "icache off" 1500

    # Запускаем ядро
    Write-Host ""
    Write-Host "`[*] Запуск ядра..." -ForegroundColor Yellow
    $serial.Write("go 0x02080000`r")
    
    # Kernel UART monitor + interactive stdin (restored after file parse fix)
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Green
    Write-Host "  Kernel running; UART mirror + console" -ForegroundColor Green
    Write-Host "========================================" -ForegroundColor Green
    Write-Host ""

    $kernelOutput = ""
    while ($true) {
        try {
            $ch = [char]$serial.ReadChar()
            Write-Host -NoNewline $ch
            $kernelOutput += $ch
            if ($kernelOutput -match 'Operational|H-Exo Omni-Core') {
                Write-Host ""
                Write-Host "`[+] KERNEL BOOT OK" -ForegroundColor Green -BackgroundColor Black
                Write-Host ""
                break
            }
        } catch {
            Start-Sleep -Milliseconds 10
        }
    }

    Write-Host "[INPUT] keys sent to board; Ctrl+C exits (closes port in finally)" -ForegroundColor Magenta
    while ($serial.IsOpen) {
        try {
            if ($serial.BytesToRead -gt 0) {
                Write-Host -NoNewline ([char]$serial.ReadChar())
            }
            if ([Console]::KeyAvailable) {
                $key = [Console]::ReadKey($true)
                $c = $key.KeyChar
                if ($key.Key -eq [ConsoleKey]::C -and $key.Modifiers -band [ConsoleModifiers]::Control) {
                    Write-Host "`n`[!] Ctrl+C" -ForegroundColor Yellow
                    break
                }
                if ($c) {
                    $serial.Write($c)
                    Write-Host "`n`[>] sent: $c" -ForegroundColor Green
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
        Write-Host "`[!] Port closed" -ForegroundColor Yellow
    }
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Deploy finished." -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
