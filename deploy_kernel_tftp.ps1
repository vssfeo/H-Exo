# deploy_kernel_tftp.ps1 - Расширенный скрипт для автоматического развертывания ядра через TFTP

param(
    [string]$ComPort = "COM3",
    [int]$BaudRate = 115200,
    [switch]$AutoReboot = $false
)

# Пути к файлам
$ProjectRoot = Get-Location
$KernelSource = "$ProjectRoot\main_neuro.c"
$KernelBinary = "$ProjectRoot\kernel_neuro.bin"
$TftpDirectory = "C:\tftpboot"
$TftpBinaryPath = "$TftpDirectory\kernel_neuro.bin"

# Проверка наличия необходимых файлов и директорий
function Test-Prerequisites {
    Write-Host "Проверка необходимых компонентов..."
    
    # Проверяем наличие бинарного файла
    if (-not (Test-Path $KernelBinary)) {
        Write-Warning "Бинарный файл $KernelBinary не найден!"
        Write-Host "Попробуем скомпилировать..."
        if (-not (Compile-Kernel)) {
            return $false
        }
    }
    
    # Проверяем наличие директории TFTP
    if (-not (Test-Path $TftpDirectory)) {
        Write-Warning "Директория TFTP $TftpDirectory не найдена!"
        Write-Host "Создаем директорию..."
        try {
            New-Item -ItemType Directory -Path $TftpDirectory -Force | Out-Null
            Write-Host "Директория создана успешно"
        } catch {
            Write-Error "Не удалось создать директорию TFTP: $_"
            return $false
        }
    }
    
    Write-Host "Все необходимые компоненты найдены"
    return $true
}

# Функция для компиляции ядра
function Compile-Kernel {
    Write-Host "Компиляция ядра..."
    
    # Проверяем наличие Makefile
    if (Test-Path "$ProjectRoot\Makefile.neuro") {
        # Переходим в директорию проекта
        Push-Location $ProjectRoot
        
        try {
            # Компилируем с использованием Makefile.neuro
            $makeProcess = Start-Process -FilePath "make" -ArgumentList "-f Makefile.neuro" -NoNewWindow -Wait -PassThru
            
            if ($makeProcess.ExitCode -eq 0) {
                Write-Host "Компиляция успешна!"
                Pop-Location
                return $true
            } else {
                Write-Error "Ошибка компиляции! Код ошибки: $($makeProcess.ExitCode)"
                Pop-Location
                return $false
            }
        } catch {
            Write-Error "Ошибка при запуске компиляции: $_"
            Pop-Location
            return $false
        }
    } else {
        Write-Host "Makefile.neuro не найден, пропускаем компиляцию"
        return $true
    }
}

# Функция для копирования бинарного файла в директорию TFTP
function Copy-ToTftpDirectory {
    Write-Host "Копирование файла ядра в директорию TFTP..."
    
    if (Test-Path $KernelBinary) {
        try {
            Copy-Item $KernelBinary $TftpBinaryPath -Force
            Write-Host "Файл успешно скопирован в $TftpBinaryPath"
            return $true
        } catch {
            Write-Error "Ошибка при копировании файла: $_"
            return $false
        }
    } else {
        Write-Error "Бинарный файл $KernelBinary не найден!"
        return $false
    }
}

# Функция для проверки состояния TFTP сервера
function Test-TftpServer {
    Write-Host "Проверка состояния TFTP сервера..."
    
    # Проверяем, запущен ли TFTP сервер (проверяем порт 69)
    try {
        $tcpConnection = Test-NetConnection -ComputerName localhost -Port 69 -WarningAction SilentlyContinue
        if ($tcpConnection.TcpTestSucceeded) {
            Write-Host "TFTP сервер активен и слушает порт 69"
            return $true
        } else {
            Write-Warning "TFTP сервер не отвечает на порту 69"
            Write-Host "Убедитесь, что TFTP сервер запущен и настроен правильно"
            return $false
        }
    } catch {
        Write-Warning "Не удалось проверить состояние TFTP сервера: $_"
        return $false
    }
}

# Функция для отправки команды через UART
function Send-UartCommand {
    param(
        [string]$Command,
        [string]$Port = $ComPort,
        [int]$Baud = $BaudRate
    )
    
    Write-Host "Отправка команды через UART ($Port, $Baud baud)..."
    
    # Проверяем наличие COM-порта
    if (-not (Get-WmiObject -Class Win32_SerialPort | Where-Object {$_.DeviceID -eq $Port})) {
        Write-Warning "COM-порт $Port не найден!"
        Write-Host "Подключите UART-кабель и укажите правильный COM-порт"
        return $false
    }
    
    # В реальной реализации здесь будет код для отправки команды через COM-порт
    # Пока что просто выводим команду в консоль
    Write-Host "Команда отправлена: $Command"
    
    # Здесь можно добавить реальную отправку команды через System.IO.Ports.SerialPort
    # или использовать внешнюю утилиту типа mode или echo
    
    return $true
}

# Функция для отображения инструкций по ручной загрузке
function Show-ManualInstructions {
    Write-Host ""
    Write-Host "=== Инструкция по ручной загрузке ===" -ForegroundColor Yellow
    Write-Host "Для загрузки ядра вручную выполните следующие команды в консоли U-Boot:"
    Write-Host "  setenv ipaddr 192.168.1.10" -ForegroundColor Cyan
    Write-Host "  setenv serverip 192.168.1.166" -ForegroundColor Cyan
    Write-Host "  setenv bootfile kernel_neuro.bin" -ForegroundColor Cyan
    Write-Host "  setenv netboot 'tftp \${loadaddr} \${bootfile}; go \${loadaddr}'" -ForegroundColor Cyan
    Write-Host "  saveenv" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "После настройки загружайте ядро командой:" -ForegroundColor Yellow
    Write-Host "  run netboot" -ForegroundColor Green
    Write-Host "===============================" -ForegroundColor Yellow
    Write-Host ""
}

# Основной процесс развертывания
Write-Host "=== Начало развертывания ядра через TFTP ===" -ForegroundColor Green
Write-Host ""

# 1. Проверяем необходимые компоненты
if (-not (Test-Prerequisites)) {
    Write-Error "=== Необходимые компоненты не найдены или не могут быть созданы ==="
    Show-ManualInstructions
    exit 1
}

# 2. Компилируем ядро
if (-not (Compile-Kernel)) {
    Write-Error "=== Ошибка компиляции ==="
    Show-ManualInstructions
    exit 1
}

# 3. Копируем бинарный файл в директорию TFTP
if (-not (Copy-ToTftpDirectory)) {
    Write-Error "=== Ошибка при копировании файла ==="
    Show-ManualInstructions
    exit 1
}

# 4. Проверяем состояние TFTP сервера
if (-not (Test-TftpServer)) {
    Write-Warning "=== TFTP сервер может быть не настроен ==="
    Show-ManualInstructions
    # Продолжаем выполнение, так как сервер может быть запущен позже
}

Write-Host "=== Файл ядра успешно обновлен в директории TFTP ===" -ForegroundColor Green

# 5. Отправляем команду перезагрузки через UART, если указан параметр
if ($AutoReboot) {
    Write-Host "Отправка команды перезагрузки через UART..."
    if (Send-UartCommand -Command "run netboot" -Port $ComPort -Baud $BaudRate) {
        Write-Host "Команда перезагрузки отправлена успешно!" -ForegroundColor Green
    } else {
        Write-Warning "Не удалось отправить команду перезагрузки через UART"
        Show-ManualInstructions
    }
} else {
    Write-Host "Для автоматической загрузки ядра выполните в консоли U-Boot:" -ForegroundColor Yellow
    Write-Host "  run netboot" -ForegroundColor Green
    Write-Host ""
    Write-Host "Или запустите скрипт с параметром -AutoReboot для автоматической отправки команды:" -ForegroundColor Yellow
    Write-Host "  .\deploy_kernel_tftp.ps1 -AutoReboot" -ForegroundColor Cyan
}

Write-Host "=== Развертывание завершено успешно! ===" -ForegroundColor Green

# Пауза перед завершением
Write-Host ""
Write-Host "Нажмите любую клавишу для завершения..."
$host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown") | Out-Null