# test_tftp_setup_fixed.ps1 - Исправленный скрипт для проверки настройки TFTP

Write-Host "=== Проверка настройки TFTP ===" -ForegroundColor Green
Write-Host ""

# 1. Проверяем наличие необходимых файлов
Write-Host "1. Проверка наличия необходимых файлов:" -ForegroundColor Yellow

$requiredFiles = @(
    "deploy_kernel_tftp.ps1",
    "start_tftp_server.ps1",
    "Tftpd64.ini",
    "TFTP_SETUP_INSTRUCTIONS.md",
    "README_TFTP_SETUP.md"
)

foreach ($file in $requiredFiles) {
    if (Test-Path $file) {
        Write-Host "  [OK] $file" -ForegroundColor Green
    } else {
        Write-Host "  [MISSING] $file" -ForegroundColor Red
    }
}

Write-Host ""

# 2. Проверяем директорию TFTP
Write-Host "2. Проверка директории TFTP:" -ForegroundColor Yellow

$tftpDir = "C:\tftpboot"
if (Test-Path $tftpDir) {
    Write-Host "  [OK] Директория TFTP найдена: $tftpDir" -ForegroundColor Green
    
    # Проверяем наличие kernel_neuro.bin
    $kernelFile = "$tftpDir\kernel_neuro.bin"
    if (Test-Path $kernelFile) {
        Write-Host "  [OK] Файл ядра найден: $kernelFile" -ForegroundColor Green
        
        # Получаем размер файла
        $fileInfo = Get-Item $kernelFile
        Write-Host "  [INFO] Размер файла: $($fileInfo.Length) байт" -ForegroundColor Cyan
    } else {
        Write-Host "  [WARNING] Файл ядра не найден в директории TFTP" -ForegroundColor Yellow
    }
} else {
    Write-Host "  [ERROR] Директория TFTP не найдена: $tftpDir" -ForegroundColor Red
}

Write-Host ""

# 3. Проверяем сетевые настройки
Write-Host "3. Проверка сетевых настроек:" -ForegroundColor Yellow

# Получаем IP-адреса
try {
    $ipAddresses = Get-NetIPAddress | Where-Object {$_.AddressFamily -eq "IPv4" -and $_.InterfaceAlias -notlike "*Loopback*"}
    
    foreach ($ip in $ipAddresses) {
        Write-Host "  [INTERFACE] $($ip.InterfaceAlias)" -ForegroundColor Cyan
        Write-Host "    IP: $($ip.IPAddress)/$($ip.PrefixLength)" -ForegroundColor Cyan
        
        # Проверяем, является ли это нашим серверным IP
        if ($ip.IPAddress -eq "192.168.1.166") {
            Write-Host "    [MATCH] Это IP-адрес TFTP сервера" -ForegroundColor Green
        }
    }
} catch {
    Write-Host "  [INFO] Не удалось получить сетевые настройки" -ForegroundColor Cyan
}

Write-Host ""

Write-Host "=== Проверка завершена ===" -ForegroundColor Green