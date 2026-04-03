# ps_tftp_server.ps1 - Simple PowerShell TFTP Server

# Check if running as administrator
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole] "Administrator")
if (-not $isAdmin) {
    Write-Host "This script needs to run as Administrator to bind to port 69" -ForegroundColor Red
    Write-Host "Please run PowerShell as Administrator and try again" -ForegroundColor Yellow
    exit 1
}

Write-Host "=== PowerShell TFTP Server ===" -ForegroundColor Green
Write-Host "TFTP Directory: C:\tftpboot" -ForegroundColor Cyan
Write-Host "Listening on port 69" -ForegroundColor Cyan
Write-Host "Press Ctrl+C to stop" -ForegroundColor Yellow
Write-Host ""

# Create UDP listener
$udp = New-Object System.Net.Sockets.UdpClient(69)
$udp.Client.ReceiveTimeout = 5000  # 5 second timeout

# Set up TFTP directory
$tftpDir = "C:\tftpboot"
if (-not (Test-Path $tftpDir)) {
    Write-Host "Creating TFTP directory..." -ForegroundColor Yellow
    New-Item -ItemType Directory -Path $tftpDir -Force | Out-Null
}

Write-Host "TFTP Server is ready!" -ForegroundColor Green
Write-Host "" 

try {
    while ($true) {
        # Wait for incoming request
        $remoteEndPoint = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
        
        try {
            $data = $udp.Receive([ref]$remoteEndPoint)
        } catch [System.Net.Sockets.SocketException] {
            # Timeout - continue waiting
            continue
        }
        
        Write-Host "Received request from $($remoteEndPoint.Address):$($remoteEndPoint.Port)" -ForegroundColor Cyan
        
        # Parse TFTP opcode (big-endian)
        $opcode = ($data[0] -shl 8) -bor $data[1]
        
        if ($opcode -eq 1) {  # RRQ - Read Request
            # Extract filename (null-terminated string after opcode)
            $filenameBytes = @()
            for ($i = 2; $i -lt $data.Length; $i++) {
                if ($data[$i] -eq 0) { break }
                $filenameBytes += $data[$i]
            }
            $filename = [System.Text.Encoding]::ASCII.GetString($filenameBytes)
            
            Write-Host "Read request for file: $filename" -ForegroundColor Yellow
            
            # Check if file exists
            $filePath = Join-Path $tftpDir $filename
            if (Test-Path $filePath) {
                Write-Host "File found: $filePath" -ForegroundColor Green
                
                # Read file content
                $fileContent = [System.IO.File]::ReadAllBytes($filePath)
                Write-Host "File size: $($fileContent.Length) bytes" -ForegroundColor Cyan
                
                # Send file in 512-byte blocks
                $blockSize = 512
                $blockNum = 1
                $offset = 0
                
                $sendFinalEmpty = ($fileContent.Length % $blockSize) -eq 0
                
                while ($offset -lt $fileContent.Length -or $sendFinalEmpty) {
                    $remaining = $fileContent.Length - $offset
                    $chunkSize = [Math]::Min($blockSize, $remaining)
                    
                    # Create DATA packet: opcode(3) + block# + data
                    $dataPacket = [byte[]]::new(4 + $chunkSize)
                    $dataPacket[0] = 0
                    $dataPacket[1] = 3  # DATA opcode
                    $dataPacket[2] = ($blockNum -shr 8) -band 0xFF
                    $dataPacket[3] = $blockNum -band 0xFF
                    if ($chunkSize -gt 0) {
                        [System.Array]::Copy($fileContent, $offset, $dataPacket, 4, $chunkSize)
                    }
                    
                    # Send data packet
                    $udp.Send($dataPacket, $dataPacket.Length, $remoteEndPoint) | Out-Null
                    Write-Host "Sent block $blockNum ($chunkSize bytes)" -ForegroundColor Gray
                    
                    # Wait for ACK with retries
                    $udp.Client.ReceiveTimeout = 1000
                    $retries = 0
                    $maxRetries = 5
                    $ackReceived = $false
                    
                    while ($retries -lt $maxRetries -and -not $ackReceived) {
                        try {
                            $ackData = $udp.Receive([ref]$remoteEndPoint)
                            $ackOpcode = ($ackData[0] -shl 8) -bor $ackData[1]
                            $ackBlock = ($ackData[2] -shl 8) -bor $ackData[3]
                            
                            if ($ackOpcode -eq 4 -and $ackBlock -eq $blockNum) {
                                Write-Host "Received ACK for block $blockNum" -ForegroundColor Gray
                                $ackReceived = $true
                                $offset += $chunkSize
                                $blockNum++
                                if ($chunkSize -lt $blockSize) {
                                    $sendFinalEmpty = $false
                                    break
                                }
                            } elseif ($ackOpcode -eq 4 -and $ackBlock -lt $blockNum) {
                                # Duplicate ACK - ignore and wait for correct one
                                Write-Host "Duplicate ACK for block $ackBlock (expected $blockNum)" -ForegroundColor Yellow
                                continue
                            } else {
                                Write-Host "Invalid ACK: opcode=$ackOpcode block=$ackBlock expected=$blockNum" -ForegroundColor Yellow
                            }
                        } catch {
                            # Timeout - retransmit
                            $retries++
                            if ($retries -lt $maxRetries) {
                                Write-Host "ACK timeout for block $blockNum, retrying ($retries/$maxRetries)..." -ForegroundColor Yellow
                                $udp.Send($dataPacket, $dataPacket.Length, $remoteEndPoint) | Out-Null
                            } else {
                                Write-Host "Max retries reached for block $blockNum" -ForegroundColor Red
                                break
                            }
                        }
                    }
                    
                    if (-not $ackReceived) {
                        Write-Host "Transfer failed - no ACK for block $blockNum" -ForegroundColor Red
                        break
                    }
                }
                
                Write-Host "Transfer complete: $($fileContent.Length) bytes sent" -ForegroundColor Green
            } else {
                Write-Host "File not found: $filePath" -ForegroundColor Red
                # Send error packet
                $errorMsg = "File not found"
                $errorBytes = [System.Text.Encoding]::ASCII.GetBytes($errorMsg)
                $errorPacket = [byte[]]::new(5 + $errorBytes.Length)
                $errorPacket[0] = 0
                $errorPacket[1] = 5  # ERROR opcode
                $errorPacket[2] = 0
                $errorPacket[3] = 1  # Error code: File not found
                [System.Array]::Copy($errorBytes, 0, $errorPacket, 4, $errorBytes.Length)
                $errorPacket[$errorPacket.Length - 1] = 0
                $udp.Send($errorPacket, $errorPacket.Length, $remoteEndPoint) | Out-Null
            }
        }
    }
}
finally {
    $udp.Close()
    Write-Host "TFTP Server stopped" -ForegroundColor Yellow
}