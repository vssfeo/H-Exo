param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 1500000
)

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  H-Exo Final Boot Test" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

# Проверка порта
Write-Host "[*] Checking $PortName..." -ForegroundColor Yellow
$available = [System.IO.Ports.SerialPort]::GetPortNames()
if ($available -notcontains $PortName) {
    Write-Host "[-] $PortName not available. Available ports: $($available -join ', ')" -ForegroundColor Red
    Write-Host "[!] Please reconnect USB-UART adapter and try again" -ForegroundColor Red
    exit 1
}

Write-Host "[+] $PortName is available" -ForegroundColor Green

# Открываем порт
$port = New-Object System.IO.Ports.SerialPort $PortName, $BaudRate
$port.Parity = [System.IO.Ports.Parity]::None
$port.DataBits = 8
$port.StopBits = [System.IO.Ports.StopBits]::One
$port.Handshake = [System.IO.Ports.Handshake]::None
$port.ReadTimeout = 100
$port.DtrEnable = $false
$port.RtsEnable = $false

try {
    $port.Open()
    Write-Host "[+] Port opened at $BaudRate baud" -ForegroundColor Green
    Write-Host "`n[*] Waiting for board boot..." -ForegroundColor Yellow
    Write-Host "[*] REBOOT THE BOARD NOW!" -ForegroundColor Yellow
    Write-Host "========================================" -ForegroundColor Cyan
    
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $buffer = New-Object System.Text.StringBuilder
    $promptDetected = $false
    
    while ($sw.Elapsed.TotalSeconds -lt 90) {
        try {
            while ($port.BytesToRead -gt 0) {
                $b = $port.ReadByte()
                if ($b -ge 0) {
                    $ch = [char]$b
                    [void]$buffer.Append($ch)
                    Write-Host -NoNewline $ch -ForegroundColor Gray
                }
            }
            
            $text = $buffer.ToString()
            if ($text -match '=>\s*$') {
                $promptDetected = $true
                break
            }
            
            # Send Ctrl+C every 300ms
            if ($sw.ElapsedMilliseconds % 300 -lt 50) {
                $port.Write([char]0x03)
            }
        } catch {}
        Start-Sleep -Milliseconds 50
    }
    
    if (-not $promptDetected) {
        Write-Host "`n`n[-] U-Boot prompt not detected" -ForegroundColor Red
        exit 1
    }
    
    Write-Host "`n`n[+] U-Boot prompt detected!" -ForegroundColor Green
    Write-Host "========================================" -ForegroundColor Cyan
    
    # Send commands
    function Send-Cmd($cmd) {
        Write-Host "`n[CMD] $cmd" -ForegroundColor Yellow
        $port.WriteLine($cmd)
        Start-Sleep -Milliseconds 300
        
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        while ($sw.ElapsedMilliseconds -lt 1500) {
            try {
                while ($port.BytesToRead -gt 0) {
                    $b = $port.ReadByte()
                    if ($b -ge 0) {
                        Write-Host -NoNewline ([char]$b) -ForegroundColor Cyan
                    }
                }
            } catch {}
            Start-Sleep -Milliseconds 10
        }
    }
    
    Send-Cmd "mmc dev 1"
    Send-Cmd "mmc read 0x02080000 500000 3"
    
    Write-Host "`n[*] Launching H-Exo kernel..." -ForegroundColor Yellow
    Write-Host "========================================" -ForegroundColor Cyan
    $port.WriteLine("go 0x02080000")
    Start-Sleep -Milliseconds 500
    
    # Read H-Exo output
    $sw.Restart()
    $gotBanner = $false
    
    while ($sw.Elapsed.TotalSeconds -lt 20) {
        try {
            while ($port.BytesToRead -gt 0) {
                $b = $port.ReadByte()
                if ($b -ge 0) {
                    $ch = [char]$b
                    Write-Host -NoNewline $ch -ForegroundColor Green
                    
                    if ($ch -match 'H-Exo|Aleph') {
                        $gotBanner = $true
                    }
                }
            }
        } catch {}
        Start-Sleep -Milliseconds 10
    }
    
    Write-Host "`n========================================" -ForegroundColor Cyan
    
    if ($gotBanner) {
        Write-Host "[+] SUCCESS! H-Exo booted!" -ForegroundColor Green
    } else {
        Write-Host "[!] H-Exo banner not detected" -ForegroundColor Yellow
        Write-Host "[*] Check output above for errors" -ForegroundColor Yellow
    }
    
} catch {
    Write-Host "`n[-] Error: $_" -ForegroundColor Red
    exit 1
} finally {
    if ($port.IsOpen) {
        $port.Close()
    }
}
