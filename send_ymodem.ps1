# UART path: YMODEM (loady) -> RAM at LoadAddress, then optional mmc write to SD in the slot (see -AutoBoot).
param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 1500000,
    [string]$FilePath = "C:\Users\SERYOGA\AndroidStudioProjects\H-Exo\kernel.bin",
    [UInt32]$LoadAddress = 0x02080000,
    [switch]$AutoBoot,
    [UInt32]$KernelSector = 500000,
    [UInt32]$KernelSectorCount = 0,
    [int]$MmcDevice = 1,
    [int]$MmcWriteReadTimeoutMs = 120000,
    [int]$InterPacketDelayMs = 0,
    [int]$DataBlockTimeoutMs = 20000,
    [int]$MaxBlockRetries = 30
)

# Same table and update step as U-Boot lib/crc16-ccitt.c (crc16_ccitt(0, buf, len)).
$script:Crc16CcittTab = @(
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7, 0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52b5, 0x4294, 0x72f7, 0x62d6, 0x9339, 0x8318, 0xb37b, 0xa35a, 0xd3bd, 0xc39c, 0xf3ff, 0xe3de,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64e6, 0x74c7, 0x44a4, 0x5485, 0xa56a, 0xb54b, 0x8528, 0x9509, 0xe5ee, 0xf5cf, 0xc5ac, 0xd58d,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76d7, 0x66f6, 0x5695, 0x46b4, 0xb75b, 0xa77a, 0x9719, 0x8738, 0xf7df, 0xe7fe, 0xd79d, 0xc7bc,
    0x48c4, 0x58e5, 0x6886, 0x78a7, 0x0840, 0x1861, 0x2802, 0x3823, 0xc9cc, 0xd9ed, 0xe98e, 0xf9af, 0x8948, 0x9969, 0xa90a, 0xb92b,
    0x5af5, 0x4ad4, 0x7ab7, 0x6a96, 0x1a71, 0x0a50, 0x3a33, 0x2a12, 0xdbfd, 0xcbdc, 0xfbbf, 0xeb9e, 0x9b79, 0x8b58, 0xbb3b, 0xab1a,
    0x6ca6, 0x7c87, 0x4ce4, 0x5cc5, 0x2c22, 0x3c03, 0x0c60, 0x1c41, 0xedae, 0xfd8f, 0xcdec, 0xddcd, 0xad2a, 0xbd0b, 0x8d68, 0x9d49,
    0x7e97, 0x6eb6, 0x5ed5, 0x4ef4, 0x3e13, 0x2e32, 0x1e51, 0x0e70, 0xff9f, 0xefbe, 0xdfdd, 0xcffc, 0xbf1b, 0xaf3a, 0x9f59, 0x8f78,
    0x9188, 0x81a9, 0xb1ca, 0xa1eb, 0xd10c, 0xc12d, 0xf14e, 0xe16f, 0x1080, 0x00a1, 0x30c2, 0x20e3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83b9, 0x9398, 0xa3fb, 0xb3da, 0xc33d, 0xd31c, 0xe37f, 0xf35e, 0x02b1, 0x1290, 0x22f3, 0x32d2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xb5ea, 0xa5cb, 0x95a8, 0x8589, 0xf56e, 0xe54f, 0xd52c, 0xc50d, 0x34e2, 0x24c3, 0x14a0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xa7db, 0xb7fa, 0x8799, 0x97b8, 0xe75f, 0xf77e, 0xc71d, 0xd73c, 0x26d3, 0x36f2, 0x0691, 0x16b0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xd94c, 0xc96d, 0xf90e, 0xe92f, 0x99c8, 0x89e9, 0xb98a, 0xa9ab, 0x5844, 0x4865, 0x7806, 0x6827, 0x18c0, 0x08e1, 0x3882, 0x28a3,
    0xcb7d, 0xdb5c, 0xeb3f, 0xfb1e, 0x8bf9, 0x9bd8, 0xabbb, 0xbb9a, 0x4a75, 0x5a54, 0x6a37, 0x7a16, 0x0af1, 0x1ad0, 0x2ab3, 0x3a92,
    0xfd2e, 0xed0f, 0xdd6c, 0xcd4d, 0xbdaa, 0xad8b, 0x9de8, 0x8dc9, 0x7c26, 0x6c07, 0x5c64, 0x4c45, 0x3ca2, 0x2c83, 0x1ce0, 0x0cc1,
    0xef1f, 0xff3e, 0xcf5d, 0xdf7c, 0xaf9b, 0xbfba, 0x8fd9, 0x9ff8, 0x6e17, 0x7e36, 0x4e55, 0x5e74, 0x2e93, 0x3eb2, 0x0ed1, 0x1ef0
)

function New-Crc16Ccitt {
    param([byte[]]$Data, [int]$Count)
    [uint16]$cksum = 0
    for ($i = 0; $i -lt $Count; $i++) {
        $b = $Data[$i] -band 0xFF
        $idx = ((($cksum -shr 8) -bxor $b) -band 0xFF)
        $cksum = [uint16](($script:Crc16CcittTab[$idx] -bxor (($cksum -shl 8) -band 0xFFFF)) -band 0xFFFF)
    }
    return [int]$cksum
}

function Write-Bytes {
    param([System.IO.Ports.SerialPort]$Port, [byte[]]$Bytes)
    $Port.Write($Bytes, 0, $Bytes.Length)
    $Port.BaseStream.Flush()
}

function Read-ByteWithTimeout {
    param(
        [System.IO.Ports.SerialPort]$Port,
        [int]$TimeoutMs
    )
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        try {
            if ($Port.BytesToRead -gt 0) {
                return $Port.ReadByte()
            }
        } catch {
        }
        Start-Sleep -Milliseconds 10
    }
    return $null
}

function Read-SerialText {
    param(
        [System.IO.Ports.SerialPort]$Port,
        [int]$TimeoutMs
    )
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $buffer = New-Object System.Text.StringBuilder
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        try {
            while ($Port.BytesToRead -gt 0) {
                $b = $Port.ReadByte()
                if ($b -ge 0) {
                    [void]$buffer.Append([char]$b)
                }
            }
        } catch {
        }
        Start-Sleep -Milliseconds 20
    }
    return $buffer.ToString()
}

function Send-UbootCommand {
    param(
        [System.IO.Ports.SerialPort]$Port,
        [string]$Command,
        [int]$ReadTimeoutMs = 1500
    )
    $bytes = [Text.Encoding]::ASCII.GetBytes(($Command + "`r"))
    Write-Bytes -Port $Port -Bytes $bytes
    Start-Sleep -Milliseconds 200
    return Read-SerialText -Port $Port -TimeoutMs $ReadTimeoutMs
}

function Wait-ForUbootPrompt {
    param(
        [System.IO.Ports.SerialPort]$Port,
        [int]$TimeoutMs = 0
    )

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $buffer = New-Object System.Text.StringBuilder
    $lastProgressTime = 0
    
    while (($TimeoutMs -le 0) -or ($sw.ElapsedMilliseconds -lt $TimeoutMs)) {
        try {
            # Читаем всё доступное
            while ($Port.BytesToRead -gt 0) {
                $b = $Port.ReadByte()
                if ($b -ge 0) {
                    $ch = [char]$b
                    [void]$buffer.Append($ch)
                    Write-Host -NoNewline $ch
                }
            }
            
            $text = $buffer.ToString()
            
            # Проверяем наличие U-Boot prompt
            if ($text -match '=>' -or $text -match 'Hit any key to stop autoboot') {
                Write-Host "`n[+] U-Boot prompt detected!" -ForegroundColor Green
                return $text
            }
            
            # Отправляем Ctrl+C каждые 500ms для прерывания autoboot
            if ($sw.ElapsedMilliseconds - $lastProgressTime -gt 500) {
                $Port.Write([char]0x03)
                $lastProgressTime = $sw.ElapsedMilliseconds
            }
            
        } catch {
            Write-Host "`n[!] Error reading serial: $_" -ForegroundColor Red
        }
        Start-Sleep -Milliseconds 50
    }

    throw "Timed out waiting for the U-Boot prompt. Reboot the board and rerun the script."
}

function Wait-ForAny {
    param(
        [System.IO.Ports.SerialPort]$Port,
        [int[]]$Wanted,
        [int]$TimeoutMs
    )
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        try {
            while ($Port.BytesToRead -gt 0) {
                $b = $Port.ReadByte()
                if ($b -ge 0 -and ($Wanted -contains $b)) {
                    return $b
                }
            }
        } catch {
        }
        Start-Sleep -Milliseconds 10
    }
    return $null
}

function Send-Packet0 {
    param(
        [System.IO.Ports.SerialPort]$Port,
        [string]$Name,
        [long]$Size
    )
    $payload = New-Object byte[] 128
    $text = ([Text.Encoding]::ASCII.GetBytes(("{0}`0{1}`0" -f $Name, $Size)))
    [Array]::Copy($text, 0, $payload, 0, [Math]::Min($text.Length, $payload.Length))
    $packet = New-Object byte[] (3 + 128 + 2)
    $packet[0] = 0x01
    $packet[1] = 0x00
    $packet[2] = 0xFF
    [Array]::Copy($payload, 0, $packet, 3, 128)
    $crc = New-Crc16Ccitt -Data $payload -Count 128
    $packet[131] = [byte](($crc -shr 8) -band 0xFF)
    $packet[132] = [byte]($crc -band 0xFF)
    Write-Bytes -Port $Port -Bytes $packet
}

function Send-DataPacket {
    param(
        [System.IO.Ports.SerialPort]$Port,
        [byte[]]$Chunk,
        [int]$BlockNum
    )
    $payload = New-Object byte[] 1024
    [Array]::Copy($Chunk, 0, $payload, 0, $Chunk.Length)
    for ($i = $Chunk.Length; $i -lt 1024; $i++) {
        $payload[$i] = 0x1A
    }
    $packet = New-Object byte[] (3 + 1024 + 2)
    $packet[0] = 0x02
    $packet[1] = [byte]($BlockNum -band 0xFF)
    $packet[2] = [byte](0xFF - ($BlockNum -band 0xFF))
    [Array]::Copy($payload, 0, $packet, 3, 1024)
    $crc = New-Crc16Ccitt -Data $payload -Count 1024
    $packet[1027] = [byte](($crc -shr 8) -band 0xFF)
    $packet[1028] = [byte]($crc -band 0xFF)
    Write-Bytes -Port $Port -Bytes $packet
}

if (-not (Test-Path $FilePath)) {
    throw "Missing file: $FilePath"
}

$fileBytes = [System.IO.File]::ReadAllBytes($FilePath)
$fileName = [System.IO.Path]::GetFileName($FilePath)
$fileSize = $fileBytes.Length

if ($fileSize -lt 1) {
    throw "File is empty: $FilePath"
}
$mmcSectorCount = if ($KernelSectorCount -eq 0) {
    [UInt32][Math]::Ceiling($fileSize / 512.0)
} else {
    $KernelSectorCount
}

$port = New-Object System.IO.Ports.SerialPort $PortName, $BaudRate, 'None', 8, 'One'
$port.Handshake = 'None'
$port.NewLine = "`r`n"
$port.ReadTimeout = 200
$port.WriteTimeout = 2000
$port.ReadBufferSize = 65536
$port.WriteBufferSize = 65536
$port.DtrEnable = $false
$port.RtsEnable = $false

try {
    $port.Open()
    $port.DiscardInBuffer()
    $port.DiscardOutBuffer()

    Write-Host "[*] Waiting for U-Boot prompt..." -ForegroundColor Yellow
    $bootLog = Wait-ForUbootPrompt -Port $port
    if ($bootLog) { Write-Host $bootLog }

    Write-Host "[*] Sending loady command to $PortName at $BaudRate..." -ForegroundColor Yellow
    $cmd = "loady 0x{0:X8}`r" -f $LoadAddress
    $cmdBytes = [Text.Encoding]::ASCII.GetBytes($cmd)
    Write-Bytes -Port $port -Bytes $cmdBytes

    $c = Wait-ForAny -Port $port -Wanted @(0x43) -TimeoutMs 15000
    if ($null -eq $c) {
        throw "Did not receive YMODEM ready signal 'C' from U-Boot. Make sure the board is waiting at loady."
    }

    Write-Host "[*] Receiver ready; sending header packet..." -ForegroundColor Cyan
    Send-Packet0 -Port $port -Name $fileName -Size $fileSize
    $resp = Wait-ForAny -Port $port -Wanted @(0x06) -TimeoutMs 10000
    if ($null -eq $resp) { throw "No ACK after header packet." }
    $c = Wait-ForAny -Port $port -Wanted @(0x43) -TimeoutMs 10000
    if ($null -eq $c) { throw "No 'C' after header packet ACK." }

    # U-Boot xyzModem (CRC mode) sends NAK or another 'C' (0x43) on CRC/frame errors — not only 0x15.
    # Ignoring 0x43 caused false "No response" timeouts when the line glitched or the receiver retried.
    $block = 1
    $offset = 0
    while ($offset -lt $fileBytes.Length) {
        $remain = $fileBytes.Length - $offset
        $chunkSize = [Math]::Min(1024, $remain)
        $chunk = New-Object byte[] $chunkSize
        [Array]::Copy($fileBytes, $offset, $chunk, 0, $chunkSize)

        $tries = 0
        while ($true) {
            Send-DataPacket -Port $port -Chunk $chunk -BlockNum $block
            if ($InterPacketDelayMs -gt 0) { Start-Sleep -Milliseconds $InterPacketDelayMs }

            $resp = Wait-ForAny -Port $port -Wanted @(0x06, 0x15, 0x43) -TimeoutMs $DataBlockTimeoutMs
            if ($null -eq $resp) {
                throw "No ACK/NAK/C after data block $block (timeout ${DataBlockTimeoutMs}ms). Check baud matches U-Boot (loady addr baud), cable, or try -InterPacketDelayMs 5 -BaudRate 115200."
            }
            if ($resp -eq 0x06) { break }

            $tries++
            if ($tries -ge $MaxBlockRetries) {
                throw "Block $block failed after $MaxBlockRetries retries (last response: 0x$('{0:X2}' -f $resp))."
            }
            if ($resp -eq 0x15) {
                Write-Host "[*] NAK on block $block, resend ($tries/$MaxBlockRetries)..." -ForegroundColor Yellow
            } else {
                Write-Host "[*] Receiver sent 'C' (retry) on block $block, resending ($tries/$MaxBlockRetries)..." -ForegroundColor Yellow
            }
        }

        $offset += $chunkSize
        $block = ($block + 1) -band 0xFF
        if ($block -eq 0) { $block = 1 }
    }

    Write-Host "[*] Sending EOT..." -ForegroundColor Cyan
    $eot = [byte[]](0x04)
    Write-Bytes -Port $port -Bytes $eot
    $resp = Wait-ForAny -Port $port -Wanted @(0x15, 0x06) -TimeoutMs 10000
    if ($null -eq $resp) { throw "No response after first EOT." }
    if ($resp -eq 0x15) {
        Write-Host "[*] Receiver NAKed EOT, sending it again..." -ForegroundColor Yellow
        Write-Bytes -Port $port -Bytes $eot
        $resp = Wait-ForAny -Port $port -Wanted @(0x06) -TimeoutMs 10000
        if ($null -eq $resp) { throw "No ACK after second EOT." }
    }

    $c = Wait-ForAny -Port $port -Wanted @(0x43) -TimeoutMs 10000
    if ($null -eq $c) { throw "No 'C' after EOT ACK." }

    Write-Host "[*] Sending final empty header..." -ForegroundColor Cyan
    Send-Packet0 -Port $port -Name "" -Size 0
    $resp = Wait-ForAny -Port $port -Wanted @(0x06) -TimeoutMs 10000
    if ($null -eq $resp) { throw "No ACK after final empty header." }

    Write-Host "[+] YMODEM transfer complete for $fileName ($fileSize bytes)" -ForegroundColor Green

    if ($AutoBoot) {
        Write-Host "[*] AutoBoot: mmc write from RAM ($('0x{0:X8}' -f $LoadAddress)) -> SD (mmc dev $MmcDevice, LBA $KernelSector, $mmcSectorCount sectors)..." -ForegroundColor Yellow
        $log = Send-UbootCommand -Port $port -Command ("mmc dev {0}" -f $MmcDevice)
        if ($log) { Write-Host $log }
        $log = Send-UbootCommand -Port $port -Command ("mmc write 0x{0:X8} {1} {2}" -f $LoadAddress, $KernelSector, $mmcSectorCount) -ReadTimeoutMs $MmcWriteReadTimeoutMs
        if ($log) { Write-Host $log }
        $log = Send-UbootCommand -Port $port -Command ("go 0x{0:X8}" -f $LoadAddress) -ReadTimeoutMs 5000
        if ($log) { Write-Host $log }
    } else {
        Write-Host ("[+] Next in U-Boot: mmc dev {0}; mmc write 0x{1:X8} {2} {3}; go 0x{1:X8}" -f $MmcDevice, $LoadAddress, $KernelSector, $mmcSectorCount) -ForegroundColor Yellow
    }
}
finally {
    if ($port.IsOpen) { $port.Close() }
}
