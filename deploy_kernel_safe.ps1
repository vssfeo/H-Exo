param(
    [string]$PortName = "COM3",
    [string]$KernelPath = "kernel_neuro.bin",
    [int]$MaxRetries = 3
)

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  H-Exo Safe Kernel Deployment" -ForegroundColor Cyan
Write-Host "  YMODEM with Verification" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

# Compute kernel hash
$kernelBytes = [System.IO.File]::ReadAllBytes($KernelPath)
$kernelSize = $kernelBytes.Length
$sha256 = [System.Security.Cryptography.SHA256]::Create()
$kernelHash = [BitConverter]::ToString($sha256.ComputeHash($kernelBytes)).Replace("-", "")

Write-Host "[*] Kernel: $KernelPath" -ForegroundColor Yellow
Write-Host "[*] Size: $kernelSize bytes" -ForegroundColor Yellow
Write-Host "[*] SHA256: $kernelHash" -ForegroundColor Yellow
Write-Host "[*] REBOOT THE BOARD NOW!" -ForegroundColor Red
Write-Host ""

$port = New-Object System.IO.Ports.SerialPort $PortName, 1500000, None, 8, One
$port.Handshake = [System.IO.Ports.Handshake]::None
$port.ReadTimeout = 30000
$port.WriteTimeout = 5000

try {
    $port.Open()
    Write-Host "[+] Serial port opened at 1500000 baud" -ForegroundColor Green
    
    # Wait for U-Boot prompt
    Write-Host "[*] Waiting for U-Boot prompt..." -ForegroundColor Yellow
    $buffer = ""
    $timeout = [DateTime]::Now.AddSeconds(60)
    
    while ([DateTime]::Now -lt $timeout) {
        if ($port.BytesToRead -gt 0) {
            $data = $port.ReadExisting()
            $buffer += $data
            Write-Host $data -NoNewline
            
            # Interrupt autoboot
            if ($buffer -match "Hit any key to stop autoboot") {
                for ($i = 0; $i -lt 20; $i++) {
                    $port.Write([char]3)
                    Start-Sleep -Milliseconds 50
                }
            }
            
            if ($buffer -match "=>\s*$") {
                Write-Host "`n[+] U-Boot prompt detected!" -ForegroundColor Green
                break
            }
        }
        Start-Sleep -Milliseconds 100
    }
    
    if ($buffer -notmatch "=>") {
        Write-Host "[ERROR] U-Boot prompt not detected" -ForegroundColor Red
        exit 1
    }
    
    # YMODEM transfer with retries
    $transferSuccess = $false
    
    for ($attempt = 1; $attempt -le $MaxRetries; $attempt++) {
        Write-Host "`n[*] Transfer attempt $attempt of $MaxRetries..." -ForegroundColor Yellow
        
        # Send loady command
        $port.WriteLine("loady 0x02080000")
        Start-Sleep -Seconds 2
        
        # Clear buffer
        if ($port.BytesToRead -gt 0) {
            $port.ReadExisting() | Out-Null
        }
        
        # YMODEM transfer
        Write-Host "[*] Starting YMODEM transfer (slower, safer)..." -ForegroundColor Yellow
        
        $SOH = 0x01
        $EOT = 0x04
        $ACK = 0x06
        $NAK = 0x15
        $CAN = 0x18
        
        # Wait for 'C' (CRC mode request)
        $cReceived = $false
        $startTime = [DateTime]::Now
        while (([DateTime]::Now - $startTime).TotalSeconds -lt 10) {
            if ($port.BytesToRead -gt 0) {
                $byte = $port.ReadByte()
                if ($byte -eq 0x43) {  # 'C'
                    $cReceived = $true
                    Write-Host "[+] Receiver ready (CRC mode)" -ForegroundColor Green
                    break
                }
            }
            Start-Sleep -Milliseconds 50
        }
        
        if (-not $cReceived) {
            Write-Host "[WARN] No CRC request received, retrying..." -ForegroundColor Yellow
            continue
        }
        
        # Send header packet (filename + size)
        $header = New-Object byte[] 133
        $header[0] = $SOH
        $header[1] = 0x00
        $header[2] = 0xFF
        
        $filename = [System.Text.Encoding]::ASCII.GetBytes("kernel.bin")
        [Array]::Copy($filename, 0, $header, 3, $filename.Length)
        
        $sizeStr = [System.Text.Encoding]::ASCII.GetBytes($kernelSize.ToString())
        [Array]::Copy($sizeStr, 0, $header, 3 + $filename.Length + 1, $sizeStr.Length)
        
        # CRC16
        $crc = 0
        for ($i = 3; $i -lt 131; $i++) {
            $crc = $crc -bxor ($header[$i] -shl 8)
            for ($j = 0; $j -lt 8; $j++) {
                if ($crc -band 0x8000) {
                    $crc = (($crc -shl 1) -bxor 0x1021) -band 0xFFFF
                } else {
                    $crc = ($crc -shl 1) -band 0xFFFF
                }
            }
        }
        $header[131] = ($crc -shr 8) -band 0xFF
        $header[132] = $crc -band 0xFF
        
        $port.Write($header, 0, 133)
        Write-Host "[*] Header sent" -ForegroundColor Gray
        
        # Wait for ACK (with longer timeout)
        $ackReceived = $false
        $startTime = [DateTime]::Now
        while (([DateTime]::Now - $startTime).TotalSeconds -lt 5) {
            if ($port.BytesToRead -gt 0) {
                $byte = $port.ReadByte()
                if ($byte -eq $ACK) {
                    $ackReceived = $true
                    break
                } elseif ($byte -eq $NAK -or $byte -eq $CAN) {
                    Write-Host "[WARN] Receiver NAK/CAN on header" -ForegroundColor Yellow
                    break
                }
            }
            Start-Sleep -Milliseconds 50
        }
        
        if (-not $ackReceived) {
            Write-Host "[WARN] No ACK for header, retrying..." -ForegroundColor Yellow
            continue
        }
        
        # Wait for 'C' for data
        $cReceived = $false
        $startTime = [DateTime]::Now
        while (([DateTime]::Now - $startTime).TotalSeconds -lt 5) {
            if ($port.BytesToRead -gt 0) {
                $byte = $port.ReadByte()
                if ($byte -eq 0x43) {
                    $cReceived = $true
                    break
                }
            }
            Start-Sleep -Milliseconds 50
        }
        
        if (-not $cReceived) {
            Write-Host "[WARN] No CRC request for data, retrying..." -ForegroundColor Yellow
            continue
        }
        
        # Send data blocks (128 bytes each, SLOWER)
        $blockNum = 1
        $offset = 0
        $allBlocksSuccess = $true
        
        while ($offset -lt $kernelSize) {
            $blockSize = [Math]::Min(128, $kernelSize - $offset)
            $block = New-Object byte[] 133
            
            $block[0] = $SOH
            $block[1] = $blockNum -band 0xFF
            $block[2] = (255 - $blockNum) -band 0xFF
            
            [Array]::Copy($kernelBytes, $offset, $block, 3, $blockSize)
            
            # Pad with 0x1A if needed
            for ($i = $blockSize; $i -lt 128; $i++) {
                $block[3 + $i] = 0x1A
            }
            
            # CRC16
            $crc = 0
            for ($i = 3; $i -lt 131; $i++) {
                $crc = $crc -bxor ($block[$i] -shl 8)
                for ($j = 0; $j -lt 8; $j++) {
                    if ($crc -band 0x8000) {
                        $crc = (($crc -shl 1) -bxor 0x1021) -band 0xFFFF
                    } else {
                        $crc = ($crc -shl 1) -band 0xFFFF
                    }
                }
            }
            $block[131] = ($crc -shr 8) -band 0xFF
            $block[132] = $crc -band 0xFF
            
            # Send block
            $port.Write($block, 0, 133)
            
            # SLOWER: Wait longer between blocks
            Start-Sleep -Milliseconds 100
            
            # Wait for ACK
            $ackReceived = $false
            $startTime = [DateTime]::Now
            while (([DateTime]::Now - $startTime).TotalSeconds -lt 2) {
                if ($port.BytesToRead -gt 0) {
                    $byte = $port.ReadByte()
                    if ($byte -eq $ACK) {
                        $ackReceived = $true
                        break
                    } elseif ($byte -eq $NAK) {
                        Write-Host "[WARN] NAK on block $blockNum" -ForegroundColor Yellow
                        break
                    }
                }
                Start-Sleep -Milliseconds 20
            }
            
            if (-not $ackReceived) {
                Write-Host "[ERROR] No ACK for block $blockNum" -ForegroundColor Red
                $allBlocksSuccess = $false
                break
            }
            
            if ($blockNum % 10 -eq 0) {
                $progress = [Math]::Round(($offset / $kernelSize) * 100)
                Write-Host "[*] Progress: $progress% (block $blockNum)" -ForegroundColor Gray
            }
            
            $blockNum++
            $offset += $blockSize
        }
        
        if (-not $allBlocksSuccess) {
            Write-Host "[WARN] Transfer incomplete, retrying..." -ForegroundColor Yellow
            continue
        }
        
        # Send EOT
        $port.WriteByte($EOT)
        Start-Sleep -Milliseconds 500
        
        # Wait for ACK
        $ackReceived = $false
        $startTime = [DateTime]::Now
        while (([DateTime]::Now - $startTime).TotalSeconds -lt 3) {
            if ($port.BytesToRead -gt 0) {
                $byte = $port.ReadByte()
                if ($byte -eq $ACK) {
                    $ackReceived = $true
                    break
                }
            }
            Start-Sleep -Milliseconds 50
        }
        
        if ($ackReceived) {
            Write-Host "[+] YMODEM transfer complete!" -ForegroundColor Green
            $transferSuccess = $true
            break
        } else {
            Write-Host "[WARN] No final ACK, retrying..." -ForegroundColor Yellow
        }
    }
    
    if (-not $transferSuccess) {
        Write-Host "[ERROR] Transfer failed after $MaxRetries attempts" -ForegroundColor Red
        exit 1
    }
    
    # Verify with CRC32
    Write-Host "`n[*] Verifying kernel integrity..." -ForegroundColor Yellow
    Start-Sleep -Seconds 1
    
    $port.WriteLine("crc32 0x02080000 0x$($kernelSize.ToString('X'))")
    Start-Sleep -Seconds 2
    
    $output = ""
    if ($port.BytesToRead -gt 0) {
        $output = $port.ReadExisting()
        Write-Host $output -ForegroundColor Gray
    }
    
    # Boot kernel
    Write-Host "[*] Booting kernel..." -ForegroundColor Yellow
    $port.WriteLine("go 0x02080000")
    Start-Sleep -Seconds 1
    
    # Switch to 115200 for kernel output
    $port.Close()
    Start-Sleep -Milliseconds 500
    
    $port = New-Object System.IO.Ports.SerialPort $PortName, 115200, None, 8, One
    $port.Open()
    
    Write-Host "[*] Monitoring kernel output at 115200 baud..." -ForegroundColor Yellow
    Write-Host "[*] Looking for emergency beacons: A B C D E" -ForegroundColor Yellow
    Write-Host ""
    
    # Monitor output
    $beacons = ""
    while ($true) {
        if ($port.BytesToRead -gt 0) {
            $data = $port.ReadExisting()
            Write-Host $data -NoNewline
            
            # Track beacons
            if ($data -match "[ABCDE]") {
                foreach ($char in $data.ToCharArray()) {
                    if ($char -match "[ABCDE]" -and $beacons -notlike "*$char*") {
                        $beacons += $char
                        Write-Host "`n[!] BEACON $char detected!" -ForegroundColor Green
                    }
                }
            }
            
            if ($data -match "H-Exo") {
                Write-Host "`n[!] H-Exo kernel booted!" -ForegroundColor Green
            }
        }
        Start-Sleep -Milliseconds 50
    }
    
} catch {
    Write-Host "`n[ERROR] $($_.Exception.Message)" -ForegroundColor Red
} finally {
    if ($null -ne $port -and $port.IsOpen) {
        $port.Close()
    }
}
