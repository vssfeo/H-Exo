# tftp_server_robust.ps1 - Надёжный TFTP сервер с авто-перезапуском
param(
    [string]$TftpDir = "C:\tftpboot",
    [int]$Port = 69,
    [switch]$AutoRestart = $true,
    [int]$MaxRestarts = 10
)

$ErrorActionPreference = "Continue"
$restartCount = 0
$totalTransfers = 0
$successTransfers = 0

function Write-Log {
    param([string]$Message, [string]$Level = "INFO")
    $timestamp = Get-Date -Format "HH:mm:ss"
    $color = switch ($Level) {
        "ERROR" { "Red" }
        "WARN"  { "Yellow" }
        "SUCCESS" { "Green" }
        default { "White" }
    }
    Write-Host "[$timestamp] [$Level] $Message" -ForegroundColor $color
    Add-Content -Path "tftp_server.log" -Value "[$timestamp] [$Level] $Message" -ErrorAction SilentlyContinue
}

function Start-TftpServer {
    param()
    
    # Проверка прав администратора
    $isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole] "Administrator")
    if (-not $isAdmin) {
        Write-Log "Need Administrator rights for port 69" "ERROR"
        return $false
    }
    
    # Проверка директории
    if (-not (Test-Path $TftpDir)) {
        New-Item -ItemType Directory -Path $TftpDir -Force | Out-Null
        Write-Log "Created TFTP directory: $TftpDir" "INFO"
    }
    
    # Проверка файла ядра
    $kernelPath = Join-Path $TftpDir "kernel_neuro.bin"
    if (-not (Test-Path $kernelPath)) {
        Write-Log "Kernel not found: $kernelPath" "ERROR"
        return $false
    }
    
    $fileSize = (Get-Item $kernelPath).Length
    Write-Log "Kernel ready: $fileSize bytes" "INFO"
    
    try {
        # Создаём UDP listener
        $udp = New-Object System.Net.Sockets.UdpClient
        $udp.Client.SetSocketOption([System.Net.Sockets.SocketOptionLevel]::Socket, [System.Net.Sockets.SocketOptionName]::ReuseAddress, $true)
        $udp.Client.Bind([System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, $Port))
        $udp.Client.ReceiveTimeout = 5000
        
        Write-Log "TFTP Server started on port $Port" "SUCCESS"
        Write-Log "Serving directory: $TftpDir" "INFO"
        Write-Log "Press Ctrl+C to stop" "INFO"
        
        $running = $true
        $currentTransfer = $null
        
        while ($running) {
            try {
                $remoteEndPoint = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
                
                try {
                    $data = $udp.Receive([ref]$remoteEndPoint)
                } catch {
                    # Timeout или нет данных - это нормально
                    if ($_.Exception.Message -match "timed out|время|timeout|WSAETIMEDOUT|10060") {
                        continue
                    }
                    # Сброс соединения тоже нормально для UDP
                    if ($_.Exception.Message -match "reset|отклик|connection|WSAECONNRESET|10054") {
                        continue
                    }
                    throw
                }
                
                $clientIP = $remoteEndPoint.Address
                $clientPort = $remoteEndPoint.Port
                
                # Parse TFTP opcode
                $opcode = ([int]$data[0] -shl 8) -bor [int]$data[1]
                
                switch ($opcode) {
                    1 { # RRQ - Read Request
                        $filenameBytes = @()
                        $modeBytes = @()
                        $stage = 0
                        
                        for ($i = 2; $i -lt $data.Length; $i++) {
                            if ($data[$i] -eq 0) {
                                $stage++
                                if ($stage -eq 2) { break }
                                continue
                            }
                            if ($stage -eq 0) {
                                $filenameBytes += $data[$i]
                            } elseif ($stage -eq 1) {
                                $modeBytes += $data[$i]
                            }
                        }
                        
                        $filename = [System.Text.Encoding]::ASCII.GetString($filenameBytes)
                        $mode = [System.Text.Encoding]::ASCII.GetString($modeBytes)
                        
                        if ([string]::IsNullOrWhiteSpace($filename)) {
                            Write-Log "Empty filename from $clientIP" "WARN"
                            Send-Error -UdpClient $udp -EndPoint $remoteEndPoint -Code 0 -Message "Empty filename"
                            continue
                        }
                        
                        Write-Log "RRQ from $clientIP`:$clientPort for '$filename'" "INFO"
                        
                        $filePath = Join-Path $TftpDir $filename
                        
                        if (-not (Test-Path $filePath)) {
                            Write-Log "File not found: $filename" "ERROR"
                            Send-Error -UdpClient $udp -EndPoint $remoteEndPoint -Code 1 -Message "File not found"
                            continue
                        }
                        
                        # Запускаем передачу в отдельном runspace
                        $fileContent = [System.IO.File]::ReadAllBytes($filePath)
                        Write-Log "Sending $filename ($($fileContent.Length) bytes) to $clientIP" "INFO"
                        
                        $transfer = @{
                            Udp = $udp
                            Client = $remoteEndPoint
                            Data = $fileContent
                            BlockSize = 512
                            BlockNum = 1
                            Offset = 0
                            Retries = 0
                            MaxRetries = 5
                            StartTime = Get-Date
                        }
                        
                        # Отправляем первый блок
                        Send-DataBlock -Transfer $transfer
                        $currentTransfer = $transfer
                        $totalTransfers++
                    }
                    
                    4 { # ACK
                        if ($currentTransfer -and ($clientIP -eq $currentTransfer.Client.Address)) {
                            $blockNum = ([int]$data[2] -shl 8) -bor [int]$data[3]
                            
                            if ($blockNum -eq $currentTransfer.BlockNum) {
                                # ACK получен, следующий блок
                                $currentTransfer.Offset += $currentTransfer.BlockSize
                                $currentTransfer.BlockNum++
                                $currentTransfer.Retries = 0
                                
                                if ($currentTransfer.Offset -lt $currentTransfer.Data.Length) {
                                    Send-DataBlock -Transfer $currentTransfer
                                } else {
                                    # Передача завершена
                                    $duration = ((Get-Date) - $currentTransfer.StartTime).TotalSeconds
                                    $speed = [Math]::Round($currentTransfer.Data.Length / $duration / 1024, 1)
                                    Write-Log "Transfer complete: $($currentTransfer.Data.Length) bytes in ${duration}s (${speed} KiB/s)" "SUCCESS"
                                    $successTransfers++
                                    $currentTransfer = $null
                                }
                            }
                        }
                    }
                    
                    5 { # ERROR
                        $errorCode = ([int]$data[2] -shl 8) -bor [int]$data[3]
                        Write-Log "Client error code $errorCode from $clientIP" "ERROR"
                        $currentTransfer = $null
                    }
                }
            } catch {
                # Не логируем ожидаемые ошибки таймаута и соединения
                $errMsg = $_.Exception.Message
                $fullErr = $_.ToString()
                if (($errMsg -match "timed out|время|timeout|WSAETIMEDOUT|10060|reset|отклик|connection|WSAECONNRESET|10054") -or
                    ($fullErr -match "timed out|время|timeout|WSAETIMEDOUT|10060|reset|отклик|connection|WSAECONNRESET|10054")) {
                    # Ожидаемая ошибка для UDP - игнорируем тихо
                } else {
                    Write-Log "Error: $_" "ERROR"
                }
                $currentTransfer = $null
            }
        }
        
        $udp.Close()
        return $true
        
    } catch {
        Write-Log "Fatal error: $_" "ERROR"
        return $false
    }
}

function Send-DataBlock {
    param([hashtable]$Transfer)
    
    $remaining = $Transfer.Data.Length - $Transfer.Offset
    $chunkSize = [Math]::Min($Transfer.BlockSize, $remaining)
    
    $packet = [byte[]]::new(4 + $chunkSize)
    $packet[0] = 0
    $packet[1] = 3  # DATA
    $packet[2] = ([int]$Transfer.BlockNum -shr 8) -band 0xFF
    $packet[3] = [int]$Transfer.BlockNum -band 0xFF
    
    if ($chunkSize -gt 0) {
        [Array]::Copy($Transfer.Data, $Transfer.Offset, $packet, 4, $chunkSize)
    }
    
    $Transfer.Udp.Send($packet, $packet.Length, $Transfer.Client) | Out-Null
}

function Send-Error {
    param($UdpClient, $EndPoint, [int]$Code, [string]$Message)
    
    $msgBytes = [System.Text.Encoding]::ASCII.GetBytes($Message)
    $packet = [byte[]]::new(4 + $msgBytes.Length + 1)
    $packet[0] = 0
    $packet[1] = 5  # ERROR
    $packet[2] = 0
    $packet[3] = $Code
    [Array]::Copy($msgBytes, 0, $packet, 4, $msgBytes.Length)
    $packet[$packet.Length - 1] = 0
    
    $UdpClient.Send($packet, $packet.Length, $EndPoint) | Out-Null
}

# Main loop with auto-restart
Write-Log "=== TFTP Server Robust v2.0 ===" "INFO"

while ($AutoRestart -and $restartCount -lt $MaxRestarts) {
    $restartCount++
    Write-Log "Starting server (attempt $restartCount/$MaxRestarts)" "INFO"
    
    $result = Start-TftpServer
    
    if ($result) {
        Write-Log "Server stopped normally" "INFO"
        break
    } else {
        Write-Log "Server crashed, restarting in 2 seconds..." "WARN"
        Start-Sleep -Seconds 2
    }
}

Write-Log "=== Statistics ===" "INFO"
Write-Log "Total transfers: $totalTransfers" "INFO"
Write-Log "Successful: $successTransfers" "INFO"
Write-Log "Failed: $($totalTransfers - $successTransfers)" "INFO"
