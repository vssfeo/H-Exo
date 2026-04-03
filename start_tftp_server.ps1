# start_tftp_server.ps1 - Скрипт для запуска TFTP сервера

# Проверяем, установлен ли Tftpd64
$Tftpd64Path = "C:\Program Files\Tftpd64\tftpd64.exe"
$Tftpd32Path = "C:\Program Files\Tftpd32\tftpd32.exe"
$TftpdPortablePath = "$PSScriptRoot\tftpd64.exe"

# Функция для загрузки Tftpd64
function Download-Tftpd64 {
    Write-Host "Загрузка Tftpd64..."
    
    # URL для загрузки Tftpd64 (официальный сайт)
    $downloadUrl = "https://bitbucket.org/phjounin/tftpd64/downloads/Tftpd64-4.64-setup.exe"
    $installerPath = "$env:TEMP\Tftpd64-setup.exe"
    
    try {
        # Загружаем установочный файл
        Invoke-WebRequest -Uri $downloadUrl -OutFile $installerPath
        Write-Host "Установочный файл загружен в $installerPath"
        
        # Запускаем установку
        Write-Host "Запуск установки Tftpd64..."
        Start-Process -FilePath $installerPath -ArgumentList "/S" -Wait
        
        Write-Host "Tftpd64 установлен успешно!"
        return $true
    } catch {
        Write-Error "Ошибка при загрузке или установке Tftpd64: $_"
        return $false
    }
}

# Функция для запуска TFTP сервера
function Start-TftpServer {
    # Проверяем наличие Tftpd64 в различных местах
    if (Test-Path $Tftpd64Path) {
        $tftpExecutable = $Tftpd64Path
    } elseif (Test-Path $Tftpd32Path) {
        $tftpExecutable = $Tftpd32Path
    } elseif (Test-Path $TftpdPortablePath) {
        $tftpExecutable = $TftpdPortablePath
    } else {
        Write-Host "Tftpd64 не найден, пытаемся загрузить..."
        if (Download-Tftpd64) {
            if (Test-Path $Tftpd64Path) {
                $tftpExecutable = $Tftpd64Path
            } else {
                Write-Error "Не удалось найти Tftpd64 после установки"
                return $false
            }
        } else {
            Write-Error "Не удалось загрузить Tftpd64"
            return $false
        }
    }
    
    # Запускаем TFTP сервер
    Write-Host "Запуск TFTP сервера: $tftpExecutable"
    
    try {
        # Запускаем Tftpd64 с конфигурационным файлом
        $configPath = "$PSScriptRoot\Tftpd64.ini"
        if (Test-Path $configPath) {
            Start-Process -FilePath $tftpExecutable -ArgumentList "-i $configPath" -WindowStyle Minimized
        } else {
            Start-Process -FilePath $tftpExecutable -WindowStyle Minimized
        }
        
        Write-Host "TFTP сервер запущен успешно!" -ForegroundColor Green
        return $true
    } catch {
        Write-Error "Ошибка при запуске TFTP сервера: $_"
        return $false
    }
}

# Основной процесс
Write-Host "=== Запуск TFTP сервера ===" -ForegroundColor Green

if (Start-TftpServer) {
    Write-Host "TFTP сервер успешно запущен и слушает порт 69" -ForegroundColor Green
    Write-Host "Директория TFTP: C:\tftpboot" -ForegroundColor Cyan
    Write-Host "IP-адрес сервера: 192.168.1.166" -ForegroundColor Cyan
} else {
    Write-Error "Не удалось запустить TFTP сервер"
    Write-Host "Попробуйте вручную запустить Tftpd64 и настроить его на директорию C:\tftpboot" -ForegroundColor Yellow
}

Write-Host ""