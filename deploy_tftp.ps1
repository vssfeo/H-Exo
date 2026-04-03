# deploy_tftp.ps1 - Скрипт для автоматического развертывания ядра через TFTP

# Пути к файлам
$KernelSource = "main_neuro.c"
$KernelBinary = "kernel_neuro.bin"
$TftpDirectory = "C:\tftpboot"
$TftpBinaryPath = "$TftpDirectory\$KernelBinary"

# Функция для компиляции ядра
function Compile-Kernel {
    Write-Host "Компиляция ядра..."
    
    # Проверяем наличие Makefile
    if (Test-Path "Makefile.neuro") {
        # Компилируем с использованием Makefile.neuro
        $makeResult = & make -f Makefile.neuro
        if ($LASTEXITCODE -eq 0) {
            Write-Host "Компиляция успешна!"
            return $true
        } else {
            Write-Host "Ошибка компиляции!"
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
            Write-Host "Ошибка при копировании файла: $_"
            return $false
        }
    } else {
        Write-Host "Бинарный файл $KernelBinary не найден!"
        return $false
    }
}

# Функция для отправки команды перезагрузки через UART (если подключен)
function Send-RebootCommand {
    Write-Host "Отправка команды перезагрузки через UART..."
    
    # Проверяем наличие подключения по UART (предполагаем, что оно через COM3)
    # В реальной ситуации здесь нужно определить правильный COM-порт
    $comPort = "COM3"
    
    try {
        # Отправляем команду "run netboot" через UART
        # В реальной реализации здесь будет код для работы с COM-портом
        Write-Host "Команда 'run netboot' отправлена через $comPort"
        return $true
    } catch {
        Write-Host "Не удалось отправить команду через UART: $_"
        Write-Host "Вы можете вручную выполнить 'run netboot' в консоли U-Boot"
        return $false
    }
}

# Основной процесс развертывания
Write-Host "=== Начало развертывания ядра через TFTP ==="

# 1. Компилируем ядро
if (Compile-Kernel) {
    # 2. Копируем бинарный файл в директорию TFTP
    if (Copy-ToTftpDirectory) {
        Write-Host "Файл ядра успешно обновлен в директории TFTP"
        
        # 3. Отправляем команду перезагрузки (опционально)
        # Send-RebootCommand
        
        Write-Host "=== Развертывание завершено успешно! ==="
        Write-Host "Теперь вы можете выполнить 'run netboot' в консоли U-Boot NanoPi"
    } else {
        Write-Host "=== Ошибка при копировании файла ==="
    }
} else {
    Write-Host "=== Ошибка компиляции ==="
}

# Пауза перед завершением
Write-Host "Нажмите любую клавишу для завершения..."
$host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")