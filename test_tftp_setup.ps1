# test_tftp_setup.ps1 - Скрипт для проверки настройки TFTP

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
$ipAddresses = Get-NetIPAddress | Where-Object {$_.AddressFamily -eq "IPv4" -and $_.InterfaceAlias -notlike "*Loopback*"}

foreach ($ip in $ipAddresses) {
    Write-Host "  [INTERFACE] $($ip.InterfaceAlias)" -ForegroundColor Cyan
    Write-Host "    IP: $($ip.IPAddress)/$($ip.PrefixLength)" -ForegroundColor Cyan
    
    # Проверяем, является ли это нашим серверным IP
    if ($ip.IPAddress -eq "192.168.1.166") {
        Write-Host "    [MATCH] Это IP-адрес TFTP сервера" -ForegroundColor Green
    }
}

Write-Host ""

# 4. Проверяем, слушает ли что-нибудь порт 69 (TFTP)
Write-Host "4. Проверка порта TFTP (69):" -ForegroundColor Yellow

try {
    $portCheck = Get-NetTCPConnection -LocalPort 69 -ErrorAction SilentlyContinue
    if ($portCheck) {
        Write-Host "  [OK] Порт 69 используется:" -ForegroundColor Green
        foreach ($conn in $portCheck) {
            Write-Host "    $($conn.OwningProcess) ($($conn.State))" -ForegroundColor Green
        }
    } else {
        Write-Host "  [INFO] Порт 69 не используется TCP соединениями" -ForegroundColor Cyan
        Write-Host "  TFTP использует UDP, поэтому это нормально" -ForegroundColor Cyan
    }
} catch {
    Write-Host "  [INFO] Не удалось проверить TCP порт 69" -ForegroundColor Cyan
}

Write-Host ""

# 5. Предложения по дальнейшим действиям
Write-Host "5. Рекомендации:" -ForegroundColor Yellow
Write-Host "  1. Запустите TFTP сервер:" -ForegroundColor Cyan
Write-Host "     .\start_tftp_server.ps1" -ForegroundColor Gray
Write-Host "  2. Разверните ядро:" -ForegroundColor Cyan
Write-Host "     .\deploy_kernel_tftp.ps1" -ForegroundColor Gray
Write-Host "  3. Настройте NanoPi M4 (однократно в U-Boot):" -ForegroundColor Cyan
Write-Host "     setenv ipaddr 192.168.1.10" -ForegroundColor Gray
Write-Host "     setenv serverip 192.168.1.166" -ForegroundColor Gray
Write-Host "     setenv bootfile kernel_neuro.bin" -ForegroundColor Gray
Write-Host "     setenv netboot 'tftp \${loadaddr} \${bootfile}; go \${loadaddr}'" -ForegroundColor Gray
Write-Host "     saveenv" -ForegroundColor Gray
Write-Host "  4. Загрузите ядро:" -ForegroundColor Cyan
Write-Host "     run netboot" -ForegroundColor Gray

Write-Host ""
Write-Host "=== Проверка завершена ===" -ForegroundColor Green