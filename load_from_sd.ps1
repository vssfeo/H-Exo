param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 1500000,
    [UInt32]$LoadAddress = 0x02080000,
    [UInt32]$KernelSector = 1000000,  # Far beyond Armbian partitions
    [UInt32]$KernelSectorCount = 3
)

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  H-Exo: Load & Boot from SD Card" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

$port = New-Object System.IO.Ports.SerialPort $PortName, $BaudRate
$port.Parity = [System.IO.Ports.Parity]::None
$port.DataBits = 8
$port.StopBits = [System.IO.Ports.StopBits]::One
$port.Handshake = [System.IO.Ports.Handshake]::None
$port.ReadTimeout = 100
$port.DtrEnable = $false
$port.RtsEnable = $false

function Read-UntilPrompt {
    param([System.IO.Ports.SerialPort]$Port, [int]$TimeoutSeconds = 60)
    
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $buffer = New-Object System.Text.StringBuilder
    
    while ($sw.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        try {
            while ($Port.BytesToRead -gt 0) {
                $b = $Port.ReadByte()
                if ($b -ge 0) {
                    $ch = [char]$b
                    [void]$buffer.Append($ch)
                    Write-Host -NoNewline $ch -ForegroundColor Gray
                }
            }
            
            $text = $buffer.ToString()
            if ($text -match '=>\s*$') {
                return $true
            }
            
            if ($sw.ElapsedMilliseconds % 300 -lt 50) {
                $Port.Write([char]0x03)
            }
        } catch {}
        Start-Sleep -Milliseconds 50
    }
    return $false
}

function Send-Command {
    param([System.IO.Ports.SerialPort]$Port, [string]$Cmd, [int]$ReadMs = 1500)
    
    Write-Host "`n[CMD] $Cmd" -ForegroundColor Yellow
    $Port.WriteLine($Cmd)
    Start-Sleep -Milliseconds 300
    
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $ReadMs) {
        try {
            while ($Port.BytesToRead -gt 0) {
                $b = $Port.ReadByte()
                if ($b -ge 0) {
                    Write-Host -NoNewline ([char]$b) -ForegroundColor Cyan
                }
            }
        } catch {}
        Start-Sleep -Milliseconds 10
    }
}

try {
    $port.Open()
    Write-Host "[+] Port opened at $BaudRate baud" -ForegroundColor Green
    Write-Host "[*] Waiting for U-Boot prompt..." -ForegroundColor Yellow
    Write-Host "[*] Reboot the board now!" -ForegroundColor Yellow
    Write-Host "========================================" -ForegroundColor Cyan
    
    if (-not (Read-UntilPrompt -Port $port)) {
        Write-Host "`n[!] U-Boot prompt not detected" -ForegroundColor Red
        exit 1
    }
    
    Write-Host "`n`n[+] U-Boot prompt detected!" -ForegroundColor Green
    Write-Host "========================================" -ForegroundColor Cyan
    
    # Загружаем kernel с SD
    Send-Command -Port $port -Cmd "mmc dev 1" -ReadMs 1000
    Send-Command -Port $port -Cmd "mmc read 0x$($LoadAddress.ToString('X8')) $KernelSector $KernelSectorCount" -ReadMs 2000
    
    Write-Host "`n[*] Launching H-Exo kernel..." -ForegroundColor Yellow
    Send-Command -Port $port -Cmd "go 0x$($LoadAddress.ToString('X8'))" -ReadMs 1000
    
    Write-Host "`n========================================" -ForegroundColor Cyan
    Write-Host "  H-Exo Kernel Output" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan
    
    # Читаем вывод H-Exo
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $gotBanner = $false
    
    while ($sw.Elapsed.TotalSeconds -lt 15) {
        try {
            while ($port.BytesToRead -gt 0) {
                $b = $port.ReadByte()
                if ($b -ge 0) {
                    $ch = [char]$b
                    Write-Host -NoNewline $ch -ForegroundColor Green
                    
                    if ($ch -match 'H-Exo') {
                        $gotBanner = $true
                    }
                }
            }
        } catch {}
        Start-Sleep -Milliseconds 10
    }
    
    Write-Host "`n========================================" -ForegroundColor Cyan
    
    if ($gotBanner) {
        Write-Host "[+] H-Exo boot successful!" -ForegroundColor Green
    } else {
        Write-Host "[!] H-Exo banner not detected" -ForegroundColor Red
    }
    
} catch {
    Write-Host "`n[!] Error: $_" -ForegroundColor Red
    exit 1
} finally {
    if ($port.IsOpen) {
        $port.Close()
    }
}
