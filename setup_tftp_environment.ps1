# setup_tftp_environment.ps1 - Скрипт для полной настройки TFTP окружения


Write-Host "=== Полная настройка TFTP окружения ===" -ForegroundColor Green
Write-Host ""

# Создаем директорию TFTP, если она не существует
$tftpDir = "C:\tftpboot"
if (-not (Test-Path $tftpDir)) {
    Write-Host "Создание директории TFTP: $tftpDir" -ForegroundColor Yellow
    try {
        New-Item -ItemType Directory -Path $tftpDir -Force | Out-Null
        Write-Host "  [OK] Директория создана" -ForegroundColor Green
    } catch {
        Write-Error "  [ERROR] Не удалось создать директорию: $_"
        exit 1
    }
} else {
    Write-Host "  [OK] Директория TFTP уже существует: $tftpDir" -ForegroundColor Green
}

# Копируем kernel_neuro.bin в директорию TFTP, если файл существует
$kernelSource = "$PSScriptRoot\kernel_neuro.bin"
$kernelDest = "$tftpDir\kernel_neuro.bin"

if (Test-Path $kernelSource) {
    Write-Host "Копирование файла ядра в директорию TFTP..." -ForegroundColor Yellow
    try {
        Copy-Item $kernelSource $kernelDest -Force
        Write-Host "  [OK] Файл ядра скопирован" -ForegroundColor Green
        
        # Получаем размер файла
        $fileInfo = Get-Item $kernelDest
        Write-Host "  [INFO] Размер файла: $($fileInfo.Length) байт" -ForegroundColor Cyan
    } catch {
        Write-Warning "  [WARNING] Не удалось скопировать файл ядра: $_"
    }
} else {
    Write-Host "  [INFO] Файл ядра не найден в корне проекта" -ForegroundColor Cyan
    Write-Host "  Вы можете вручную скопировать kernel_neuro.bin в $tftpDir после компиляции" -ForegroundColor Cyan
}

Write-Host ""

# Проверяем наличие конфигурационного файла Tftpd64
$tftpConfig = "$PSScriptRoot\Tftpd64.ini"
if (Test-Path $tftpConfig) {
    Write-Host "  [OK] Найден конфигурационный файл Tftpd64" -ForegroundColor Green
} else {
    Write-Host "  [INFO] Конфигурационный файл Tftpd64 не найден" -ForegroundColor Cyan
    Write-Host "  Используйте стандартные настройки Tftpd64 или создайте файл вручную" -ForegroundColor Cyan
}

Write-Host ""

# Выводим информацию о настройке
Write-Host "=== Информация о настройке ===" -ForegroundColor Yellow
Write-Host "IP-адрес TFTP сервера: 192.168.1.166" -ForegroundColor Cyan
Write-Host "Директория TFTP: C:\tftpboot" -ForegroundColor Cyan
Write-Host "Файл ядра: kernel_neuro.bin" -ForegroundColor Cyan
Write-Host ""

# Выводим инструкции по запуску
Write-Host "=== Следующие шаги ===" -ForegroundColor Yellow
Write-Host "1. Запустите TFTP сервер:" -ForegroundColor Cyan
Write-Host "   .\start_tftp_server.ps1" -ForegroundColor Gray
Write-Host ""
Write-Host "2. Настройте NanoPi M4 (однократно в U-Boot):" -ForegroundColor Cyan
Write-Host "   setenv ipaddr 192.168.1.10" -ForegroundColor Gray
Write-Host "   setenv serverip 192.168.1.166" -ForegroundColor Gray
Write-Host "   setenv bootfile kernel_neuro.bin" -ForegroundColor Gray
Write-Host "   setenv netboot 'tftp \${loadaddr} \${bootfile}; go \${loadaddr}'" -ForegroundColor Gray
Write-Host "   saveenv" -ForegroundColor Gray
Write-Host ""
Write-Host "3. Разверните и загрузите ядро:" -ForegroundColor Cyan
Write-Host "   .\deploy_kernel_tftp.ps1" -ForegroundColor Gray
Write-Host "   Затем в U-Boot выполните: run netboot" -ForegroundColor Gray

Write-Host ""
Write-Host "=== Настройка завершена ===" -ForegroundColor Green
Write-Host "Теперь вы можете значительно ускорить цикл разработки!" -ForegroundColor Green