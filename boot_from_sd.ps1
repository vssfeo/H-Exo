param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 1500000
)

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  H-Exo Boot from SD (Direct)" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

Write-Host "[*] This script will boot kernel_neuro.bin from SD sector 500000" -ForegroundColor Yellow
Write-Host "[*] REBOOT THE BOARD NOW!" -ForegroundColor Yellow
Write-Host ""

$port = New-Object System.IO.Ports.SerialPort $PortName, $BaudRate, None, 8, One
$port.Handshake = [System.IO.Ports.Handshake]::None
$port.ReadTimeout = 30000
$port.WriteTimeout = 5000

try {
    $port.Open()
    Write-Host "[*] Serial port opened at $BaudRate baud" -ForegroundColor Green
    
    # Wait for U-Boot prompt
    Write-Host "[*] Waiting for U-Boot prompt..." -ForegroundColor Yellow
    $buffer = ""
    $timeout = [DateTime]::Now.AddSeconds(60)
    $promptDetected = $false
    
    while ([DateTime]::Now -lt $timeout) {
        if ($port.BytesToRead -gt 0) {
            $data = $port.ReadExisting()
            $buffer += $data
            Write-Host $data -NoNewline
            
            # Interrupt autoboot
            if ($buffer -match "Hit any key to stop autoboot" -or $buffer -match "autoboot") {
                for ($i = 0; $i -lt 10; $i++) {
                    $port.Write([char]3)
                    Start-Sleep -Milliseconds 50
                }
            }
            
            # Check for prompt
            if ($buffer -match "=>\s*$") {
                $promptDetected = $true
                Write-Host "`n[+] U-Boot prompt detected!" -ForegroundColor Green
                break
            }
        }
        Start-Sleep -Milliseconds 100
    }
    
    if (-not $promptDetected) {
        Write-Host "[ERROR] U-Boot prompt not detected" -ForegroundColor Red
        exit 1
    }
    
    # Send boot commands
    Write-Host "[*] Sending boot commands..." -ForegroundColor Yellow
    
    $port.WriteLine("mmc dev 1")
    Start-Sleep -Seconds 1
    
    $port.WriteLine("mmc read 0x02080000 500000 28")
    Start-Sleep -Seconds 2
    
    $port.WriteLine("go 0x02080000")
    Start-Sleep -Seconds 1
    
    Write-Host "[+] Boot commands sent!" -ForegroundColor Green
    Write-Host "[*] Kernel should be booting..." -ForegroundColor Yellow
    Write-Host ""
    
    # Switch to 115200 for kernel output
    $port.Close()
    Start-Sleep -Milliseconds 500
    
    $port = New-Object System.IO.Ports.SerialPort $PortName, 115200, None, 8, One
    $port.Open()
    
    Write-Host "[*] Switched to 115200 baud for kernel output" -ForegroundColor Green
    Write-Host "[*] Monitoring output (Ctrl+C to stop)..." -ForegroundColor Yellow
    Write-Host ""
    
    # Monitor output
    while ($true) {
        if ($port.BytesToRead -gt 0) {
            $data = $port.ReadExisting()
            Write-Host $data -NoNewline
        }
        Start-Sleep -Milliseconds 50
    }
    
} catch {
    Write-Host "`n[ERROR] $($_.Exception.Message)" -ForegroundColor Red
} finally {
    if ($port.IsOpen) {
        $port.Close()
    }
}
